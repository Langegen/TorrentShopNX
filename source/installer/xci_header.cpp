#include "xci_header.h"
#include "../utils/log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

namespace installer {

// Utilities for little-endian decoding
static uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t readU64(const uint8_t* p) {
    return static_cast<uint64_t>(p[0])
         | (static_cast<uint64_t>(p[1]) << 8)
         | (static_cast<uint64_t>(p[2]) << 16)
         | (static_cast<uint64_t>(p[3]) << 24)
         | (static_cast<uint64_t>(p[4]) << 32)
         | (static_cast<uint64_t>(p[5]) << 40)
         | (static_cast<uint64_t>(p[6]) << 48)
         | (static_cast<uint64_t>(p[7]) << 56);
}

static std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static bool hasExtension(const std::string& name, const std::string& ext) {
    if (name.size() < ext.size()) return false;
    return toLower(name.substr(name.size() - ext.size())) == toLower(ext);
}

constexpr size_t XCI_HEADER_SIZE = 0x200;
constexpr size_t HFS0_MAGIC = 0x30534648; // "HFS0"

// Structure defining a parsed Hfs0 Partition
struct Hfs0Partition {
    std::string name;
    uint64_t offset;
    uint64_t size;
};

// Parses a HFS0 header from the absolute position in the source
static bool parseHfs0Header(datasource::IDataSource* source, uint64_t hfs0_offset, std::vector<Hfs0Partition>& out_partitions, uint64_t& data_region_abs) {
    uint8_t header[16];
    if (source->read(hfs0_offset, header, 16) != 16) return false;

    if (std::memcmp(header, "HFS0", 4) != 0) {
        util::logLine("XciHeader: missing HFS0 magic");
        return false;
    }

    uint32_t file_count = readU32(header + 4);
    uint32_t string_table_sz = readU32(header + 8);

    size_t header_sz = 16 + file_count * 0x40 + string_table_sz;
    data_region_abs = hfs0_offset + header_sz;

    std::vector<uint8_t> file_entries(file_count * 0x40);
    if (file_count > 0 && source->read(hfs0_offset + 16, file_entries.data(), file_entries.size()) != file_entries.size()) {
        return false;
    }

    std::vector<uint8_t> string_table(string_table_sz);
    if (string_table_sz > 0 && source->read(hfs0_offset + 16 + file_entries.size(), string_table.data(), string_table_sz) != string_table_sz) {
        return false;
    }

    out_partitions.clear();
    for (uint32_t i = 0; i < file_count; ++i) {
        const uint8_t* entry_ptr = file_entries.data() + i * 0x40;
        Hfs0Partition part;
        part.offset = readU64(entry_ptr + 0);
        part.size = readU64(entry_ptr + 8);
        uint32_t name_offset = readU32(entry_ptr + 16);

        if (name_offset < string_table_sz) {
            const char* str_ptr = reinterpret_cast<const char*>(string_table.data() + name_offset);
            size_t max_len = string_table_sz - name_offset;
            size_t len = 0;
            while (len < max_len && str_ptr[len] != '\0') len++;
            part.name = std::string(str_ptr, len);
        }

        out_partitions.push_back(part);
    }

    return true;
}

static NspEntryType classifyEntry(const std::string& name) {
    std::string lower = toLower(name);
    if (hasExtension(lower, ".cnmt.nca") || hasExtension(lower, ".cnmt.ncz")) return NspEntryType::CnmtNca;
    if (hasExtension(lower, ".nca") || hasExtension(lower, ".ncz")) return NspEntryType::Nca;
    if (hasExtension(lower, ".tik")) return NspEntryType::Ticket;
    if (hasExtension(lower, ".cert")) return NspEntryType::Cert;
    return NspEntryType::Other;
}

#ifdef __SWITCH__
static bool parseContentId(const std::string& name, NcmContentId& out) {
    std::memset(&out, 0, sizeof(out));
    std::string lower = toLower(name);
    size_t dot = lower.find('.');
    if (dot == std::string::npos || dot < 32) {
        if (dot == std::string::npos) dot = lower.size();
    }
    std::string hex_str = lower.substr(0, dot);
    if (hex_str.size() != 32) return false;

    for (char c : hex_str) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }

    for (size_t i = 0; i < 16; ++i) {
        char hi = hex_str[i * 2];
        char lo = hex_str[i * 2 + 1];
        auto hexVal = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            return 0;
        };
        out.c[i] = static_cast<uint8_t>((hexVal(hi) << 4) | hexVal(lo));
    }
    return true;
}
#endif

bool XciHeader::parse(datasource::IDataSource* source, uint64_t file_size) {
    entries_.clear();
    initial_stream_pos_ = 0;

    if (!source || file_size < XCI_HEADER_SIZE) return false;

    // 1. Читаем заголовок XCI (базовый)
    uint8_t xci_header[XCI_HEADER_SIZE];
    if (source->read(0, xci_header, sizeof(xci_header)) != sizeof(xci_header)) {
        util::logLine("XciHeader: failed to read XCI header.");
        return false;
    }

    // XCI Magic "HEAD" at offset 0x100
    if (std::memcmp(xci_header + 0x100, "HEAD", 4) != 0) {
        util::logLine("XciHeader: Invalid HEAD magic!");
        return false;
    }

    uint64_t hfs0_offset = readU64(xci_header + 0x138);

    if (hfs0_offset >= file_size) {
        util::logLine("XciHeader: Invalid hfs0Offset > file_size");
        return false;
    }

    // 2. Парсим Root HFS0
    std::vector<Hfs0Partition> root_partitions;
    uint64_t root_data_region = 0;
    
    if (!parseHfs0Header(source, hfs0_offset, root_partitions, root_data_region)) {
        util::logLine("XciHeader: failed to parse root HFS0");
        return false;
    }

    // Ищем `secure` раздел
    const Hfs0Partition* secure_part = nullptr;
    for (const auto& part : root_partitions) {
        if (part.name == "secure") {
            secure_part = &part;
            break;
        }
    }

    if (!secure_part) {
        util::logLine("XciHeader: `secure` partition not found inside XCI!");
        return false;
    }

    uint64_t secure_abs_offset = root_data_region + secure_part->offset;

    if (secure_abs_offset >= file_size) {
        util::logLine("XciHeader: invalid secure partition offset");
        return false;
    }

    // 3. Парсим Secure HFS0
    std::vector<Hfs0Partition> secure_files;
    uint64_t secure_data_region = 0;
    if (!parseHfs0Header(source, secure_abs_offset, secure_files, secure_data_region)) {
        util::logLine("XciHeader: failed to parse secure HFS0");
        return false;
    }

    // Начало данных Secure раздела (где лежат сами NCA/NCZ/TIK)
    initial_stream_pos_ = secure_data_region;

    // 4. Добавляем файлы как стандартные NSP Entry.
    // Смещения в NSP Entry считаются относительно data_region, что идеально подходит для HybridNspInstaller!
    for (const auto& f : secure_files) {
        NspFileEntry entry;
        entry.name = f.name;
        entry.offset = f.offset;
        entry.size = f.size;
        entry.type = classifyEntry(entry.name);
#ifdef __SWITCH__
        if (entry.type == NspEntryType::Nca || entry.type == NspEntryType::CnmtNca) {
            parseContentId(entry.name, entry.content_id);
        }
#endif
        entries_.push_back(entry);
    }

    util::logLine("XciHeader: parsed exactly " + std::to_string(entries_.size()) + " files from secure HFS0.");
    return true;
}

bool XciHeader::hasTicket() const {
    for (const auto& e : entries_) {
        if (e.type == NspEntryType::Ticket) return true;
    }
    return false;
}

const NspFileEntry* XciHeader::findByType(NspEntryType type) const {
    for (const auto& e : entries_) {
        if (e.type == type) return &e;
    }
    return nullptr;
}

const NspFileEntry* XciHeader::findByName(const std::string& name) const {
    std::string lower_name = toLower(name);
    for (const auto& e : entries_) {
        if (toLower(e.name) == lower_name) return &e;
    }
    return nullptr;
}

} // namespace installer
