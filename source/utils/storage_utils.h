#pragma once

#include <cstdint>
#include <string>

namespace util {

// Суммарный размер всех файлов в каталоге (рекурсивно). 0, если каталога нет.
uint64_t dirSizeRecursive(const std::string& path);

// Размер одного файла или суммарный размер каталога (рекурсивно).
// 0, если пути нет.
uint64_t pathSize(const std::string& path);

// Удаляет содержимое каталога (сам каталог остаётся).
// Возвращает количество освобождённых байт.
uint64_t deleteDirContents(const std::string& path);

// Удаляет файл, возвращает освобождённые байты (0, если файла нет).
uint64_t deleteFile(const std::string& path);

// Оставшиеся в системе плейсхолдеры прерванных установок (NCM).
// storageId: 1 = SD-карта, 0 = память консоли (NAND).
bool getLeftoverPlaceholders(int storageId, int& out_count, int64_t& out_total_size);

// Удаляет ВСЕ оставшиеся плейсхолдеры в указанном хранилище.
// storageId: 1 = SD-карта, 0 = память консоли (NAND).
bool cleanupLeftoverPlaceholders(int storageId, int& out_count, int64_t& out_freed_size);

} // namespace util