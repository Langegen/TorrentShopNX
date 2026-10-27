#pragma once

// Listing an archive straight from a torrent stream without downloading it:
// the 7z header and the ZIP central directory live at the end of the file,
// so only the first and last pieces are fetched.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace datasource { class IDataSource; }

namespace util {

struct ArchiveEntryInfo {
    std::string path;     // "/" separators, no leading "/" and no trailing "/" for folders
    bool is_dir = false;
    uint64_t size = 0;    // uncompressed
};

struct ArchivePeek {
    std::string format;   // "7z" or "zip"
    std::vector<ArchiveEntryInfo> entries;
    uint64_t total_uncompressed = 0;
};

// Lists a 7z or ZIP archive read through `source` (already opened). RAR and
// other formats return false. Blocks until the needed pieces arrive, unless
// `cancel` is set.
bool peekArchiveFromSource(datasource::IDataSource* source,
                           uint64_t archive_size,
                           const std::shared_ptr<std::atomic<bool>>& cancel,
                           ArchivePeek& out,
                           std::string& out_error);

// ZIP central directory parser over a random-access reader (exposed for tests).
// `read(offset, buf, n)` must return true only after reading all n bytes.
bool parseZipDirectory(uint64_t archive_size,
                       const std::function<bool(uint64_t, void*, size_t)>& read,
                       ArchivePeek& out,
                       std::string& out_error);

// 7z listing (sevenzip_utils.cpp).
bool peek7zArchiveFromSource(datasource::IDataSource* source,
                             uint64_t archive_size,
                             const std::shared_ptr<std::atomic<bool>>& cancel,
                             ArchivePeek& out,
                             std::string& out_error);

} // namespace util
