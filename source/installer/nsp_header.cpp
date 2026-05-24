#include "nsp_header.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace installer {

// =============================================================================
// Утилиты чтения little-endian
// =============================================================================
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

/// Привести строку к нижнему регистру
static std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Проверка расширения файла (case-insensitive)
static bool hasExtension(const std::string& name, const std::string& ext) {
    if (name.size() < ext.size()) return false;
    return toLower(name.substr(name.size() - ext.size())) == toLower(ext);
}

// =============================================================================
// NspHeader::parse — основной парсинг PFS0
// =============================================================================
bool NspHeader::parse(const void* header_data, size_t size) {
    entries_.clear();
    data_region_offset_ = 0;
    file_count_ = 0;

    if (!header_data || size < 0x10) return false;

    const uint8_t* data = static_cast<const uint8_t*>(header_data);

    // Проверка магического числа "PFS0"
    if (std::memcmp(data, "PFS0", 4) != 0) {
        return false;
    }

    file_count_              = readU32(data + 4);
    uint32_t string_table_sz = readU32(data + 8);
    // data[12..15] — reserved

    // Вычисляем смещения структур
    const size_t entry_table_offset  = 0x10;
    const size_t entry_size          = 0x18; // 24 байта на запись
    const size_t string_table_offset = entry_table_offset + file_count_ * entry_size;
    data_region_offset_              = string_table_offset + string_table_sz;

    // Проверяем, хватает ли нам данных для всего заголовка
    if (size < data_region_offset_) {
        return false;
    }

    entries_.reserve(file_count_);

    for (uint32_t i = 0; i < file_count_; ++i) {
        const uint8_t* entry_ptr = data + entry_table_offset + i * entry_size;

        NspFileEntry entry;
        entry.offset = readU64(entry_ptr + 0);  // Смещение относительно data-региона
        entry.size   = readU64(entry_ptr + 8);
        uint32_t name_offset = readU32(entry_ptr + 16);
        // entry_ptr[20..23] — reserved

        // Извлекаем имя из таблицы строк
        size_t name_pos = string_table_offset + name_offset;
        if (name_pos < size) {
            for (size_t j = name_pos; j < size; ++j) {
                char c = static_cast<char>(data[j]);
                if (c == '\0') break;
                entry.name.push_back(c);
            }
        }

        // Классификация по расширению
        entry.type = classifyEntry(entry.name);

#ifdef __SWITCH__
        // Для NCA-файлов извлекаем ContentId из имени
        if (entry.type == NspEntryType::Nca || entry.type == NspEntryType::CnmtNca) {
            parseContentId(entry.name, entry.content_id);
        }
#endif

        entries_.push_back(entry);
    }

    return true;
}

// =============================================================================
// Классификация файлов
// =============================================================================
NspEntryType NspHeader::classifyEntry(const std::string& name) const {
    std::string lower = toLower(name);

    if (hasExtension(lower, ".cnmt.nca") || hasExtension(lower, ".cnmt.ncz")) {
        return NspEntryType::CnmtNca;
    }
    if (hasExtension(lower, ".nca") || hasExtension(lower, ".ncz")) {
        return NspEntryType::Nca;
    }
    if (hasExtension(lower, ".tik")) {
        return NspEntryType::Ticket;
    }
    if (hasExtension(lower, ".cert")) {
        return NspEntryType::Cert;
    }
    return NspEntryType::Other;
}

#ifdef __SWITCH__
bool NspHeader::parseContentId(const std::string& name, NcmContentId& out) const {
    // Имя NCA имеет формат: <32 hex символа>.nca или <32 hex>.cnmt.nca
    // Извлекаем первые 32 символа и конвертируем в 16 байт
    std::memset(&out, 0, sizeof(out));

    // Берём basename без расширения
    std::string lower = toLower(name);
    // Удаляем все расширения
    size_t dot = lower.find('.');
    if (dot == std::string::npos || dot < 32) {
        if (dot == std::string::npos) dot = lower.size();
    }

    std::string hex_str = lower.substr(0, dot);
    if (hex_str.size() != 32) return false;

    // Проверяем, что все символы hex
    for (char c : hex_str) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }

    // Конвертируем hex → байты
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

// =============================================================================
// Поисковые методы
// =============================================================================
bool NspHeader::hasTicket() const {
    for (const auto& e : entries_) {
        if (e.type == NspEntryType::Ticket) return true;
    }
    return false;
}

const NspFileEntry* NspHeader::findByType(NspEntryType type) const {
    for (const auto& e : entries_) {
        if (e.type == type) return &e;
    }
    return nullptr;
}

const NspFileEntry* NspHeader::findByName(const std::string& name) const {
    std::string lower_name = toLower(name);
    for (const auto& e : entries_) {
        if (toLower(e.name) == lower_name) return &e;
    }
    return nullptr;
}

} // namespace installer
