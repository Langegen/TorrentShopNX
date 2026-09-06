#pragma once

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <cstdint>

namespace util {

struct ArchiveProgress {
    float percentage = 0.0f;           // 0.0 to 100.0%
    uint64_t bytesExtracted = 0;       // Bytes extracted so far (uncompressed)
    uint64_t totalUncompressedSize = 0;// Total uncompressed size across all files (if known)
    uint64_t totalArchiveSize = 0;     // Compressed archive size on disk
    size_t entriesProcessed = 0;       // Number of files/dirs extracted
    size_t totalEntries = 0;           // Total files/dirs in archive (if known)
    std::string currentFileName;       // Name of the current entry being unpacked
};

// Returns true if filename has an archive extension (.zip, .rar, .7z, .tar, .gz, .bz2, .xz)
bool isArchiveFile(const std::string& path);

// Extracts the given archive to destination directory.
// Runs synchronously — caller should invoke via background thread (e.g. brls::async).
// Periodic progress callback is fired during unpacking.
// Extraction can be cancelled via cancelToken.
bool extractArchive(
    const std::string& archivePath,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError
);

} // namespace util
