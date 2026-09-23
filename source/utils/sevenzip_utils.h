#pragma once

#include "archive_utils.h"

namespace util {

// Returns true if the path ends with .7z (case-insensitive)
bool is7zFile(const std::string& path);

// Extracts a .7z archive using the embedded 7-Zip SDK decoder.
// Unlike the devkitPro libarchive build, this supports every method the
// official 7-Zip creates, including ARM64/ARM/x86 BCJ filters, BCJ2, PPMd,
// LZMA/LZMA2 and solid blocks, regardless of coder order inside folders.
// Runs synchronously - caller should invoke via background thread.
bool extract7zArchive(
    const std::string& archivePath,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError,
    uint64_t* outKnownUncompressedSize = nullptr,
    size_t* outKnownTotalEntries = nullptr
);

// Fast listing of 7z directory contents without decompressing file data
bool list7zArchiveFolder(
    const std::string& archivePath,
    const std::string& innerPath,
    std::vector<FileItem>& outItems,
    std::string& outError
);

// Fast calculation of total uncompressed size and total file entries for 7z archives
bool get7zArchiveTotals(
    const std::string& archivePath,
    uint64_t& outUncompressedSize,
    size_t& outTotalEntries
);

} // namespace util
