#include "cnmt_parser.h"
#include "../utils/log.h"

#include <cstdint>
#include <cstring>
#include <algorithm>

namespace installer {

// =============================================================================
// Р Р€РЎвЂљР С‘Р В»Р С‘РЎвЂљРЎвЂ№ little-endian
// =============================================================================
static uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t readU48(const uint8_t* p) {
    // 6-Р В±Р В°Р в„–РЎвЂљР С•Р Р†РЎвЂ№Р в„– РЎР‚Р В°Р В·Р СР ВµРЎР‚, little-endian
    return static_cast<uint64_t>(p[0])
         | (static_cast<uint64_t>(p[1]) << 8)
         | (static_cast<uint64_t>(p[2]) << 16)
         | (static_cast<uint64_t>(p[3]) << 24)
         | (static_cast<uint64_t>(p[4]) << 32)
         | (static_cast<uint64_t>(p[5]) << 40);
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

// =============================================================================
// Р РЋРЎвЂљРЎР‚РЎС“Р С”РЎвЂљРЎС“РЎР‚Р В° Р В·Р В°Р С–Р С•Р В»Р С•Р Р†Р С”Р В° CNMT (PackagedContentMeta)
// =============================================================================
// Offset  Size  Description
// 0x00    8     TitleId
// 0x08    4     Version
// 0x0C    1     Type (Application=0x80, Patch=0x81, AddOn=0x82)
// 0x0D    1     Reserved
// 0x0E    2     Extended header size
// 0x10    2     Content count
// 0x12    2     Content meta count
// 0x14    1     Attributes
// 0x15    3     Reserved
// 0x18    4     Required download system version
// 0x1C    4     Reserved
// ... Extended header (variable)
// ... Content entries
// ... Content meta entries

static constexpr size_t CNMT_HEADER_SIZE = 0x20;
static constexpr size_t CNMT_CONTENT_ENTRY_SIZE = 0x38; // 56 bytes per content entry

// =============================================================================
// CnmtParser::parse
// =============================================================================
bool CnmtParser::parse(const void* data, size_t size, CnmtData& out) {
    if (!data || size < CNMT_HEADER_SIZE) return false;

    const uint8_t* d = static_cast<const uint8_t*>(data);

    out.title_id          = readU64(d + 0x00);
    out.version           = readU32(d + 0x08);
    out.meta_type         = d[0x0C];
    uint16_t ext_hdr_size = readU16(d + 0x0E);
    out.extended_header_size = ext_hdr_size;
    out.content_count     = readU16(d + 0x10);
    out.content_meta_count = readU16(d + 0x12);
    out.attributes        = d[0x14];

    // Р РЋР СР ВµРЎвЂ°Р ВµР Р…Р С‘Р Вµ Р Т‘Р С• content entries = Р В·Р В°Р С–Р С•Р В»Р С•Р Р†Р С•Р С” + extended header
    size_t content_entries_offset = CNMT_HEADER_SIZE + ext_hdr_size;

    // Р СџРЎР‚Р С•Р Р†Р ВµРЎР‚РЎРЏР ВµР С, РЎвЂ¦Р Р†Р В°РЎвЂљР В°Р ВµРЎвЂљ Р В»Р С‘ Р Т‘Р В°Р Р…Р Р…РЎвЂ№РЎвЂ¦
    size_t needed = content_entries_offset + out.content_count * CNMT_CONTENT_ENTRY_SIZE;
    if (size < needed) {
        util::logLine("cnmt: insufficient data, need " + std::to_string(needed)
                       + " but have " + std::to_string(size));
        return false;
    }

    out.contents.clear();
    out.contents.reserve(out.content_count);

    for (uint16_t i = 0; i < out.content_count; ++i) {
        const uint8_t* entry = d + content_entries_offset + i * CNMT_CONTENT_ENTRY_SIZE;

        CnmtContentEntry ce;

        // Content entry layout:
        // 0x00  32  SHA-256 hash
        // 0x20  16  Content ID (NcmContentId)
        // 0x30   6  Size (48-bit LE)
        // 0x36   1  Content type
        // 0x37   1  ID offset
        std::memcpy(ce.hash, entry + 0x00, 32);
        std::memcpy(ce.content_id, entry + 0x20, 16);
        ce.size = readU48(entry + 0x30);
        ce.type = static_cast<CnmtContentType>(entry[0x36]);

        out.contents.push_back(ce);
    }

    // Р РЋР С•РЎвЂ¦РЎР‚Р В°Р Р…РЎРЏР ВµР С РЎРѓРЎвЂ№РЎР‚РЎвЂ№Р Вµ Р Т‘Р В°Р Р…Р Р…РЎвЂ№Р Вµ CNMT Р Т‘Р В»РЎРЏ Р С—Р ВµРЎР‚Р ВµР Т‘Р В°РЎвЂЎР С‘ Р Р† NCM API
    out.raw_data.assign(d, d + size);

    util::logLine("cnmt: parsed, title_id=0x" + std::to_string(out.title_id)
                   + " ver=" + std::to_string(out.version)
                   + " type=0x" + std::to_string(out.meta_type)
                   + " contents=" + std::to_string(out.content_count));

    return true;
}

// =============================================================================
// CnmtParser::extractFromNca РІР‚вЂќ РЎС“Р С—РЎР‚Р С•РЎвЂ°РЎвЂР Р…Р Р…РЎвЂ№Р в„– Р С—Р С•Р С‘РЎРѓР С” CNMT Р Р†Р Р…РЎС“РЎвЂљРЎР‚Р С‘ NCA
// =============================================================================
bool CnmtParser::extractFromNca(const void* nca_data, size_t nca_size, CnmtData& out) {
    if (!nca_data || nca_size < CNMT_HEADER_SIZE) return false;

    const uint8_t* d = static_cast<const uint8_t*>(nca_data);

    auto isValidMetaType = [](uint8_t type) {
        switch (type) {
            case 0x01: // SystemProgram
            case 0x02: // SystemData
            case 0x03: // SystemUpdate
            case 0x04: // BootImagePackage
            case 0x05: // BootImagePackageSafe
            case 0x80: // Application
            case 0x81: // Patch
            case 0x82: // AddOnContent
            case 0x83: // Delta
                return true;
            default:
                return false;
        }
    };

    auto tryParseAt = [&](size_t off) -> bool {
        if (off + CNMT_HEADER_SIZE > nca_size) return false;

        uint64_t possible_title_id = readU64(d + off);
        // TitleId приложений/патчей на Switch начинается с 0x0100...
        if ((possible_title_id >> 48) != 0x0100) return false;

        uint8_t type = d[off + 0x0C];
        if (!isValidMetaType(type)) return false;

        uint16_t ext_hdr_size = readU16(d + off + 0x0E);
        uint16_t content_count = readU16(d + off + 0x10);
        if (content_count == 0 || content_count > 1024) return false;
        if (ext_hdr_size > 0x4000) return false;

        size_t needed = CNMT_HEADER_SIZE + ext_hdr_size + content_count * CNMT_CONTENT_ENTRY_SIZE;
        if (needed > (nca_size - off)) return false;

        if (parse(d + off, needed, out)) {
            util::logLine("cnmt: found CNMT in NCA at offset 0x" + std::to_string(off));
            return true;
        }
        return false;
    };

    // Быстрый путь: типичное размещение данных после NCA header.
    for (size_t off = 0x400; off + CNMT_HEADER_SIZE <= nca_size; off += 0x200) {
        if (tryParseAt(off)) return true;
    }

    // Fallback: плотный скан для маленьких/нестандартных CNMT NCA.
    for (size_t off = 0; off + CNMT_HEADER_SIZE <= nca_size; off += 0x10) {
        if (tryParseAt(off)) return true;
    }

    util::logLine("cnmt: CNMT not found in NCA");
    return false;
}

#ifdef __SWITCH__
NcmContentMetaKey CnmtData::toKey() const {
    NcmContentMetaKey key = {};
    key.id      = title_id;
    key.version = version;

    // Р СљР В°Р С—Р С—Р С‘Р Р…Р С– РЎвЂљР С‘Р С—Р С•Р Р† CNMT РІвЂ вЂ™ NcmContentMetaType
    switch (meta_type) {
        case 0x01: key.type = NcmContentMetaType_SystemProgram; break;
        case 0x02: key.type = NcmContentMetaType_SystemData; break;
        case 0x03: key.type = NcmContentMetaType_SystemUpdate; break;
        case 0x04: key.type = NcmContentMetaType_BootImagePackage; break;
        case 0x05: key.type = NcmContentMetaType_BootImagePackageSafe; break;
        case 0x80: key.type = NcmContentMetaType_Application; break;
        case 0x81: key.type = NcmContentMetaType_Patch; break;
        case 0x82: key.type = NcmContentMetaType_AddOnContent; break;
        case 0x83: key.type = NcmContentMetaType_Delta; break;
        default:   key.type = NcmContentMetaType_Application; break;
    }

    key.install_type = NcmContentInstallType_Full;
    return key;
}
#endif

} // namespace installer
