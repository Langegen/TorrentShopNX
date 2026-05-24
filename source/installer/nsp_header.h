#pragma once
// =============================================================================
// NspHeader — парсинг заголовка NSP-пакета (PFS0).
// Извлечение списка NCA, тикетов, сертификатов и CNMT.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace installer {

/// Тип файла внутри NSP-контейнера
enum class NspEntryType {
    Nca,        ///< Nintendo Content Archive
    CnmtNca,   ///< Content Meta NCA (содержит CNMT)
    Ticket,     ///< Тикет (.tik)
    Cert,       ///< Сертификат (.cert)
    Other       ///< Прочие файлы
};

/// Описание одного файла внутри NSP
struct NspFileEntry {
    std::string name;
    uint64_t    offset      = 0; ///< Смещение относительно начала data-региона
    uint64_t    size        = 0;
    NspEntryType type       = NspEntryType::Other;

#ifdef __SWITCH__
    NcmContentId content_id = {}; ///< ID контента (для NCA)
    NcmContentType nca_type = NcmContentType_Program; ///< Тип NCA
#endif
};

/// Парсер PFS0-заголовка NSP-пакета.
/// Требует буфер с заголовочной областью (обычно первые ~64KB NSP-файла).
class NspHeader {
public:
    /// Распарсить PFS0-заголовок из буфера.
    /// @param header_data указатель на начало NSP-файла
    /// @param size        размер доступных данных (должно хватить на весь заголовок)
    /// @return true если парсинг успешен
    bool parse(const void* header_data, size_t size);

    /// Список файлов внутри NSP
    const std::vector<NspFileEntry>& entries() const { return entries_; }

    /// Смещение начала data-региона (после PFS0-заголовка + таблица имён)
    uint64_t dataRegionOffset() const { return data_region_offset_; }

    /// Количество файлов
    uint32_t fileCount() const { return file_count_; }

    /// Минимальный размер заголовка для парсинга
    size_t requiredHeaderSize() const { return data_region_offset_; }

    /// Есть ли тикет внутри NSP?
    bool hasTicket() const;

    /// Найти entry по типу
    const NspFileEntry* findByType(NspEntryType type) const;

    /// Найти entry по имени (case-insensitive)
    const NspFileEntry* findByName(const std::string& name) const;

#ifdef __SWITCH__
    /// Рекомендуемый storageId (SD по умолчанию)
    NcmStorageId suggestedStorage() const { return NcmStorageId_SdCard; }
#endif

private:
    /// Определить тип файла по расширению имени
    NspEntryType classifyEntry(const std::string& name) const;

#ifdef __SWITCH__
    /// Извлечь NcmContentId из имени NCA (первые 32 hex-символа)
    bool parseContentId(const std::string& name, NcmContentId& out) const;
#endif

    std::vector<NspFileEntry> entries_;
    uint64_t data_region_offset_ = 0;
    uint32_t file_count_         = 0;
};

} // namespace installer
