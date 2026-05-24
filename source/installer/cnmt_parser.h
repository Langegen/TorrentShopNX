#pragma once
// =============================================================================
// CnmtParser — парсинг Content Meta (CNMT) из NCA.
// CNMT содержит метаданные о приложении: title_id, версия, тип,
// список NCA-контентов и их типы.
// Нужен для вызова ncmContentMetaDatabaseSet().
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace installer {

/// Тип контента (из CNMT)
enum class CnmtContentType : uint8_t {
    Meta            = 0,
    Program         = 1,
    Data            = 2,
    Control         = 3,
    HtmlDocument    = 4,
    LegalInformation = 5,
    DeltaFragment   = 6
};

/// Запись о контенте из CNMT
struct CnmtContentEntry {
    uint8_t  hash[32] = {};       ///< SHA-256 хеш NCA
    uint8_t  content_id[16] = {}; ///< ID контента (совпадает с именем NCA)
    uint64_t size = 0;            ///< Размер в байтах (6 байт в CNMT)
    CnmtContentType type = CnmtContentType::Program;
};

/// Результат парсинга CNMT
struct CnmtData {
    uint64_t title_id   = 0;    ///< Title ID приложения
    uint32_t version    = 0;    ///< Версия
    uint8_t  meta_type  = 0;    ///< Тип (Application=0x80, Patch=0x81, AddOn=0x82)
    uint16_t extended_header_size = 0;
    uint16_t content_count = 0;
    uint16_t content_meta_count = 0;
    uint8_t  attributes = 0;

    std::vector<CnmtContentEntry> contents;

    /// Размер данных CNMT для ncmContentMetaDatabaseSet()
    size_t rawSize() const { return raw_data.size(); }
    const void* rawData() const { return raw_data.data(); }

    std::vector<uint8_t> raw_data; ///< Сырые данные CNMT для передачи в NCM

#ifdef __SWITCH__
    /// Сформировать NcmContentMetaKey из распарсенных данных
    NcmContentMetaKey toKey() const;
#endif
};

/// Парсер CNMT-структуры из сырых данных.
/// Данные берутся из CNMT NCA (после расшифровки NCA-контейнера).
class CnmtParser {
public:
    /// Распарсить CNMT из буфера.
    /// @param data  указатель на данные CNMT
    /// @param size  размер буфера
    /// @param out   результат парсинга
    /// @return true если парсинг успешен
    static bool parse(const void* data, size_t size, CnmtData& out);

    /// Попытаться извлечь CNMT из необработанного CNMT NCA.
    /// Упрощённый вариант: ищет CNMT по сигнатуре внутри NCA.
    /// @param nca_data  содержимое CNMT NCA файла
    /// @param nca_size  размер NCA
    /// @param out       результат парсинга
    /// @return true если CNMT найден и распарсен
    static bool extractFromNca(const void* nca_data, size_t nca_size, CnmtData& out);
};

} // namespace installer
