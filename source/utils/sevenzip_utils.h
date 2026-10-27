#pragma once

#include "archive_utils.h"

namespace datasource {
class IDataSource;
}

namespace util {

class ExtractionJournal;

// Returns true if the path ends with .7z (case-insensitive)
bool is7zFile(const std::string& path);

// outError value meaning "7z is fine, but let streaming libarchive extract it"
// (a folder is too big for the in-RAM SDK decoder, or uses a method only
// libarchive has, e.g. BZip2/Deflate/Zstd).
extern const char* const k7zRequiresStreaming;

// Extracts a .7z archive using the embedded 7-Zip SDK decoder (ARM64/ARM/x86
// BCJ, BCJ2, PPMd, LZMA/LZMA2), whole folders decoded in RAM.
// Right after the header is read, every folder's coder chain is checked
// against both this decoder and libarchive. If neither can handle the
// archive, it fails immediately with *outNoFallback = true, before any
// payload is read (for torrent streams: before the bulk is downloaded).
// Runs synchronously - caller should invoke via background thread.
bool extract7zArchive(
    const std::string& archivePath,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError,
    uint64_t* outKnownUncompressedSize = nullptr,
    size_t* outKnownTotalEntries = nullptr,
    PathTransformFn pathTransform = nullptr,
    ExtractionJournal* journal = nullptr,
    bool* outNoFallback = nullptr
);

// Extracts a .7z archive directly from an IDataSource stream.
bool extract7zArchiveFromSource(
    datasource::IDataSource* source,
    uint64_t archiveSize,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::shared_ptr<std::atomic<bool>> pauseToken,
    std::string& outError,
    uint64_t* outKnownUncompressedSize = nullptr,
    size_t* outKnownTotalEntries = nullptr,
    PathTransformFn pathTransform = nullptr,
    ExtractionJournal* journal = nullptr,
    bool* outNoFallback = nullptr
);

// Fast iteration of 7z directory entries without decompressing file data
bool iterate7zArchiveEntries(
    const std::string& archivePath,
    std::function<bool(const std::string& entryName, bool isDir)> callback
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
