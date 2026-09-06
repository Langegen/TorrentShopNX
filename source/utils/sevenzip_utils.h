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
    std::string& outError
);

} // namespace util
