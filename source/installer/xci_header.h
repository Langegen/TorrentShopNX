#pragma once

#include "nsp_header.h"

#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>

#include "../datasource/i_data_source.h"

namespace installer {

// Класс для извлечения информации из HFS0-контейнеров (XCI, XCZ)
class XciHeader {
public:
    XciHeader() = default;

    /// Инициализация парсера из потока с произвольным доступом.
    /// @param source Источник данных
    /// @param file_size Общий размер XCI файла
    /// @return true, если заголовки успешно прочитаны и 'secure' раздел найден.
    bool parse(datasource::IDataSource* source, uint64_t file_size);

    const std::vector<NspFileEntry>& entries() const { return entries_; }
    uint64_t fileCount() const { return static_cast<uint64_t>(entries_.size()); }

    bool hasTicket() const;
    const NspFileEntry* findByType(NspEntryType type) const;
    const NspFileEntry* findByName(const std::string& name) const;

    /// Смещение в потоке, на котором находится начало данных первого полезного файла.
    /// После отработки parse(...) поток окажется позиционирован перед первым файлом, 
    /// но нам важно знать это смещение (или мы можем просто сказать, сколько байт суммарно прочитано/пропущено).
    uint64_t getInitialStreamPos() const { return initial_stream_pos_; }

private:
    std::vector<NspFileEntry> entries_;
    uint64_t initial_stream_pos_ = 0; // Позиция потока после полного парсинга заголовков
};

} // namespace installer
