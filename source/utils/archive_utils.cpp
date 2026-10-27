#include "archive_utils.h"
#include "big_file.h"
#include "file_ops.h"
#include "log.h"
#include "sevenzip_utils.h"
#include "switch_utils.h"
#include "../datasource/i_data_source.h"
#include <thread>
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <unordered_set>
#include <vector>
#include "extraction_journal.h"

namespace util {

// stdio buffer per extracted file (one per extracting thread).
static constexpr size_t kExtractWriteBuffer = 4 * 1024 * 1024;

// 7-Zip 24.07: "7z a -t7z -mf=ARM64 -m0=LZMA2:d=4k -mhc=off probe.7z probe.bin",
// probe.bin = 64 bytes where byte[i] = (i*7+3) & 0xFF. Folder = [LZMA2, ARM64],
// the exact coder chain devkitPro's libarchive 3.7.2 rejects.
static const unsigned char kArm64Lzma2Probe7z[] = {
    0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c, 0x00, 0x04, 0x49, 0xa2, 0xce, 0xfb, 0x44, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x95, 0xd7, 0x9f, 0x66,
    0x01, 0x00, 0x3f, 0x03, 0x0a, 0x11, 0x18, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x42, 0x49, 0x50, 0x57,
    0x5e, 0x65, 0x6c, 0x73, 0x7a, 0x81, 0x88, 0x8f, 0x96, 0x9d, 0xa4, 0xab, 0xb2, 0xb9, 0xc0, 0xc7,
    0xce, 0xd5, 0xdc, 0xe3, 0xea, 0xf1, 0xf8, 0xff, 0x06, 0x0d, 0x14, 0x1b, 0x22, 0x29, 0x30, 0x37,
    0x3e, 0x45, 0x4c, 0x53, 0x5a, 0x61, 0x68, 0x6f, 0x76, 0x7d, 0x84, 0x8b, 0x92, 0x99, 0xa0, 0xa7,
    0xae, 0xb5, 0xbc, 0x00, 0x01, 0x04, 0x06, 0x00, 0x01, 0x09, 0x44, 0x00, 0x07, 0x0b, 0x01, 0x00,
    0x02, 0x21, 0x21, 0x01, 0x00, 0x01, 0x0a, 0x01, 0x00, 0x0c, 0x40, 0x40, 0x00, 0x08, 0x0a, 0x01,
    0xf0, 0xec, 0xd9, 0xcb, 0x00, 0x00, 0x05, 0x01, 0x19, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x11, 0x15, 0x00, 0x70, 0x00, 0x72, 0x00, 0x6f, 0x00, 0x62, 0x00, 0x65, 0x00, 0x2e, 0x00,
    0x62, 0x00, 0x69, 0x00, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00,
};

bool libarchiveSupports7zArm64Lzma() {
    static const bool supported = [] {
        bool ok = false;
        std::string err;
        struct archive* a = archive_read_new();
        if (a) {
            archive_read_support_format_7zip(a);
            struct archive_entry* entry = nullptr;
            if (archive_read_open_memory(a, kArm64Lzma2Probe7z, sizeof(kArm64Lzma2Probe7z)) == ARCHIVE_OK &&
                archive_read_next_header(a, &entry) == ARCHIVE_OK) {
                unsigned char buf[128];
                la_ssize_t n = archive_read_data(a, buf, sizeof(buf));
                ok = (n == 64);
                for (la_ssize_t i = 0; ok && i < n; i++) {
                    ok = buf[i] == static_cast<unsigned char>((i * 7 + 3) & 0xFF);
                }
            }
            if (!ok && archive_error_string(a)) err = archive_error_string(a);
            archive_read_free(a);
        }
        util::logLine(std::string("archive_utils: ") + archive_version_string() +
                      " 7z LZMA2+ARM64 support=" + (ok ? "1" : "0") + (err.empty() ? "" : " (" + err + ")"));
        return ok;
    }();
    return supported;
}

bool isArchiveFile(const std::string& path) {
    if (path.empty()) return false;
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto endsWith = [&lower](const std::string& suffix) {
        return lower.size() >= suffix.size() &&
               lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    return endsWith(".zip") ||
           endsWith(".rar") ||
           endsWith(".7z")  ||
           endsWith(".tar") ||
           endsWith(".gz")  ||
           endsWith(".bz2") ||
           endsWith(".xz")  ||
           endsWith(".001");
}

bool parseArchiveVirtualPath(
    const std::string& fullPath,
    std::string& outArchivePath,
    std::string& outInnerPath
) {
    outArchivePath.clear();
    outInnerPath.clear();
    if (fullPath.empty()) return false;

    static const std::vector<std::string> exts = {
        ".zip", ".rar", ".7z", ".tar", ".gz", ".bz2", ".xz"
    };

    std::string norm = fullPath;
    for (char& c : norm) if (c == '\\') c = '/';

    for (const auto& ext : exts) {
        size_t pos = 0;
        std::string lower = norm;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        while ((pos = lower.find(ext, pos)) != std::string::npos) {
            size_t afterExt = pos + ext.size();
            if (afterExt == norm.size() || norm[afterExt] == '/') {
                outArchivePath = norm.substr(0, afterExt);
                if (afterExt < norm.size() && norm[afterExt] == '/') {
                    outInnerPath = norm.substr(afterExt + 1);
                } else {
                    outInnerPath = "";
                }
                while (!outInnerPath.empty() && outInnerPath.back() == '/') {
                    outInnerPath.pop_back();
                }
                return true;
            }
            pos += ext.size();
        }
    }
    return false;
}

static std::string sanitizeEntryPath(const std::string& raw) {
    std::string clean = raw;
    for (char& c : clean) if (c == '\\') c = '/';
    while (!clean.empty() && (clean.front() == '/' || clean.front() == ' ')) {
        clean.erase(clean.begin());
    }
    size_t dotdot;
    while ((dotdot = clean.find("..")) != std::string::npos) {
        clean.replace(dotdot, 2, "__");
    }
    for (char& c : clean) {
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    return clean;
}

bool listArchiveFolder(
    const std::string& archivePath,
    const std::string& innerPath,
    std::vector<FileItem>& outItems,
    std::string& outError
) {
    outItems.clear();
    std::string normInner = innerPath;
    for (char& c : normInner) if (c == '\\') c = '/';
    while (!normInner.empty() && normInner.front() == '/') normInner.erase(normInner.begin());
    while (!normInner.empty() && normInner.back() == '/') normInner.pop_back();

    std::error_code ec;
    if (!std::filesystem::exists(archivePath, ec)) {
        outError = "Файл архива не найден: " + archivePath;
        return false;
    }

    if (is7zFile(archivePath)) {
        if (list7zArchiveFolder(archivePath, normInner, outItems, outError)) {
            return true;
        }
        // Fall back to libarchive if 7zsdk had an issue
    }

    struct archive* a = archive_read_new();
    if (!a) {
        outError = "Failed to allocate libarchive";
        return false;
    }
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    int r = archive_read_open_filename(a, archivePath.c_str(), 65536);
    if (r != ARCHIVE_OK) {
        const char* err = archive_error_string(a);
        outError = err ? err : "Failed to open archive file";
        archive_read_free(a);
        return false;
    }

    std::unordered_set<std::string> seenDirs;
    std::unordered_set<std::string> seenFiles;
    std::unordered_map<std::string, size_t> dirIndices;
    struct archive_entry* entry = nullptr;

    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        const char* rawPath = archive_entry_pathname_utf8(entry);
        if (!rawPath) rawPath = archive_entry_pathname(entry);
        if (!rawPath || !*rawPath) {
            archive_read_data_skip(a);
            continue;
        }

        std::string clean = sanitizeEntryPath(rawPath);
        if (clean.empty()) {
            archive_read_data_skip(a);
            continue;
        }

        bool isDir = (archive_entry_filetype(entry) == AE_IFDIR) ||
                     ((archive_entry_mode(entry) & 0170000) == 0040000) ||
                     (!clean.empty() && clean.back() == '/');
        while (!clean.empty() && clean.back() == '/') clean.pop_back();
        if (clean.empty()) {
            archive_read_data_skip(a);
            continue;
        }

        uint64_t fileSize = isDir ? 0 : static_cast<uint64_t>(std::max<la_int64_t>(0, archive_entry_size(entry)));
        time_t mtime = archive_entry_mtime(entry);

        if (normInner.empty()) {
            size_t slash = clean.find('/');
            if (slash == std::string::npos) {
                if (isDir) {
                    if (seenDirs.insert(clean).second) {
                        FileItem item;
                        item.name = clean;
                        item.path = archivePath + "/" + clean;
                        item.isDir = true;
                        item.size = 0;
                        item.modifiedTime = mtime;
                        dirIndices[clean] = outItems.size();
                        outItems.push_back(item);
                    } else {
                        auto it = dirIndices.find(clean);
                        if (it != dirIndices.end() && mtime > 0 && outItems[it->second].modifiedTime == 0) {
                            outItems[it->second].modifiedTime = mtime;
                        }
                    }
                } else {
                    if (seenFiles.insert(clean).second) {
                        FileItem item;
                        item.name = clean;
                        item.path = archivePath + "/" + clean;
                        item.isDir = false;
                        item.size = fileSize;
                        item.modifiedTime = mtime;
                        outItems.push_back(item);
                    }
                }
            } else {
                std::string top = clean.substr(0, slash);
                if (seenDirs.insert(top).second) {
                    FileItem dirItem;
                    dirItem.name = top;
                    dirItem.path = archivePath + "/" + top;
                    dirItem.isDir = true;
                    dirItem.size = 0;
                    dirItem.modifiedTime = 0;
                    dirIndices[top] = outItems.size();
                    outItems.push_back(dirItem);
                }
            }
        } else {
            std::string prefix = normInner + "/";
            if (clean.compare(0, prefix.size(), prefix) == 0) {
                std::string rel = clean.substr(prefix.size());
                if (rel.empty()) {
                    archive_read_data_skip(a);
                    continue;
                }
                size_t slash = rel.find('/');
                if (slash == std::string::npos) {
                    if (isDir) {
                        if (seenDirs.insert(rel).second) {
                            FileItem item;
                            item.name = rel;
                            item.path = archivePath + "/" + clean;
                            item.isDir = true;
                            item.size = 0;
                            item.modifiedTime = mtime;
                            dirIndices[rel] = outItems.size();
                            outItems.push_back(item);
                        } else {
                            auto it = dirIndices.find(rel);
                            if (it != dirIndices.end() && mtime > 0 && outItems[it->second].modifiedTime == 0) {
                                outItems[it->second].modifiedTime = mtime;
                            }
                        }
                    } else {
                        if (seenFiles.insert(rel).second) {
                            FileItem item;
                            item.name = rel;
                            item.path = archivePath + "/" + clean;
                            item.isDir = false;
                            item.size = fileSize;
                            item.modifiedTime = mtime;
                            outItems.push_back(item);
                        }
                    }
                } else {
                    std::string sub = rel.substr(0, slash);
                    if (seenDirs.insert(sub).second) {
                        FileItem dirItem;
                        dirItem.name = sub;
                        dirItem.path = archivePath + "/" + normInner + "/" + sub;
                        dirItem.isDir = true;
                        dirItem.size = 0;
                        dirItem.modifiedTime = 0;
                        dirIndices[sub] = outItems.size();
                        outItems.push_back(dirItem);
                    }
                }
            }
        }

        archive_read_data_skip(a);
    }

    archive_read_close(a);
    archive_read_free(a);

    std::sort(outItems.begin(), outItems.end(), [](const FileItem& a, const FileItem& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return a.name < b.name;
    });

    return true;
}

bool getArchiveTotals(
    const std::string& archivePath,
    uint64_t& outUncompressedSize,
    size_t& outTotalEntries
) {
    outUncompressedSize = 0;
    outTotalEntries = 0;

    if (is7zFile(archivePath)) {
        return get7zArchiveTotals(archivePath, outUncompressedSize, outTotalEntries);
    }

    struct archive* a = archive_read_new();
    if (!a) return false;

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    int r = archive_read_open_filename(a, archivePath.c_str(), 65536);
    if (r != ARCHIVE_OK) {
        archive_read_free(a);
        return false;
    }

    int filterCount = archive_filter_count(a);
    int filterCode = archive_filter_code(a, 0);
    // Compressed streaming archives like .tar.gz / .tar.xz require full decompression to scan headers.
    if (filterCount > 1 || (filterCode != ARCHIVE_FILTER_NONE && filterCode != ARCHIVE_FILTER_COMPRESS)) {
        archive_read_close(a);
        archive_read_free(a);
        return false;
    }

    struct archive_entry* entry = nullptr;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        outTotalEntries++;
        if (archive_entry_filetype(entry) != AE_IFDIR && ((archive_entry_mode(entry) & 0170000) != 0040000)) {
            la_int64_t sz = archive_entry_size(entry);
            if (sz > 0) {
                outUncompressedSize += static_cast<uint64_t>(sz);
            }
        }
        archive_read_data_skip(a);
    }

    archive_read_close(a);
    archive_read_free(a);
    return (outTotalEntries > 0);
}

bool iterateArchiveEntries(
    const std::string& archivePath,
    std::function<bool(const std::string& entryName, bool isDir)> callback
) {
    if (!callback) return false;
    if (is7zFile(archivePath)) {
        return iterate7zArchiveEntries(archivePath, callback);
    }

    struct archive* a = archive_read_new();
    if (!a) return false;

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    int r = archive_read_open_filename(a, archivePath.c_str(), 65536);
    if (r != ARCHIVE_OK) {
        archive_read_free(a);
        return false;
    }

    struct archive_entry* entry = nullptr;
    bool keepGoing = true;
    while (keepGoing && ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN)) {
        const char* rawPath = archive_entry_pathname_utf8(entry);
        if (!rawPath) rawPath = archive_entry_pathname(entry);
        if (!rawPath || !*rawPath) {
            archive_read_data_skip(a);
            continue;
        }

        std::string clean = sanitizeEntryPath(rawPath);
        if (clean.empty()) {
            archive_read_data_skip(a);
            continue;
        }

        bool isDir = (archive_entry_filetype(entry) == AE_IFDIR) ||
                     ((archive_entry_mode(entry) & 0170000) == 0040000) ||
                     (!clean.empty() && clean.back() == '/');

        keepGoing = callback(clean, isDir);
        archive_read_data_skip(a);
    }

    archive_read_close(a);
    archive_read_free(a);
    return true;
}

struct ArchiveStreamReader {
    datasource::IDataSource* source = nullptr;
    uint64_t totalSize = 0;
    uint64_t currentOffset = 0;
    std::shared_ptr<std::atomic<bool>> cancelToken;
    std::shared_ptr<std::atomic<bool>> pauseToken;
    std::vector<uint8_t> buffer;
    uint64_t totalBytesReadFromStream = 0;
    uint64_t readUs = 0;   // time spent getting data from the source (waiting for the torrent)
};

static uint64_t elapsedUs(std::chrono::steady_clock::time_point since) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - since).count());
}

static la_ssize_t streamArchiveRead(struct archive* /*a*/, void* client_data, const void** buff) {
    auto* reader = static_cast<ArchiveStreamReader*>(client_data);
    if (!reader || !reader->source) return ARCHIVE_FATAL;
    if (reader->cancelToken && reader->cancelToken->load()) return ARCHIVE_FATAL;

    if (reader->totalSize > 0 && reader->currentOffset >= reader->totalSize) {
        *buff = nullptr;
        return 0; // EOF
    }

    size_t want = reader->buffer.size();
    if (reader->totalSize > 0 && reader->currentOffset + want > reader->totalSize) {
        want = static_cast<size_t>(reader->totalSize - reader->currentOffset);
    }
    if (want == 0) {
        *buff = nullptr;
        return 0;
    }

    size_t got = 0;
    const auto readStart = std::chrono::steady_clock::now();
    while (!got && !g_appExiting.load()) {
        if (reader->cancelToken && reader->cancelToken->load()) return ARCHIVE_FATAL;
        if (reader->pauseToken && reader->pauseToken->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        got = reader->source->read(reader->currentOffset, reader->buffer.data(), want);
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
    }
    reader->readUs += elapsedUs(readStart);

    if (got > 0) {
        reader->currentOffset += got;
        reader->totalBytesReadFromStream += got;
        *buff = reader->buffer.data();
        return static_cast<la_ssize_t>(got);
    }

    return ARCHIVE_FATAL;
}

static la_int64_t streamArchiveSeek(struct archive* /*a*/, void* client_data, la_int64_t request, int whence) {
    auto* reader = static_cast<ArchiveStreamReader*>(client_data);
    if (!reader) return ARCHIVE_FATAL;
    if (reader->cancelToken && reader->cancelToken->load()) return ARCHIVE_FATAL;

    int64_t newOffset = 0;
    if (whence == SEEK_SET) {
        newOffset = request;
    } else if (whence == SEEK_CUR) {
        newOffset = static_cast<int64_t>(reader->currentOffset) + request;
    } else if (whence == SEEK_END) {
        newOffset = static_cast<int64_t>(reader->totalSize) + request;
    } else {
        return ARCHIVE_FATAL;
    }

    if (newOffset < 0 || (reader->totalSize > 0 && static_cast<uint64_t>(newOffset) > reader->totalSize)) {
        return ARCHIVE_FATAL;
    }

    reader->currentOffset = static_cast<uint64_t>(newOffset);
    return static_cast<la_int64_t>(reader->currentOffset);
}

static la_int64_t streamArchiveSkip(struct archive* /*a*/, void* client_data, la_int64_t request) {
    auto* reader = static_cast<ArchiveStreamReader*>(client_data);
    if (!reader) return 0;
    if (reader->cancelToken && reader->cancelToken->load()) return 0;

    if (reader->totalSize > 0) {
        la_int64_t remaining = static_cast<la_int64_t>(reader->totalSize - reader->currentOffset);
        if (request > remaining) request = remaining;
    }
    if (request < 0) request = 0;
    reader->currentOffset += static_cast<uint64_t>(request);
    return request;
}

static int streamArchiveClose(struct archive* /*a*/, void* /*client_data*/) {
    return ARCHIVE_OK;
}

static bool extractLibarchiveInternal(
    const std::string& archivePath,
    datasource::IDataSource* source,
    uint64_t streamArchiveSize,
    const std::string& destinationDir,
    const std::string& singleEntryFilter,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::shared_ptr<std::atomic<bool>> pauseToken,
    std::string& outError,
    uint64_t knownUncompressedSize = 0,
    size_t knownTotalEntries = 0,
    PathTransformFn pathTransform = nullptr,
    ExtractionJournal* journal = nullptr
) {
    auto makeDirs = [journal](const std::string& dir) {
        if (journal) journal->createDirectories(dir);
        else safeCreateDirectories(dir);
    };

    // Where the time goes (logged at the end): waiting for data, writing, unpacking.
    const auto extractStart = std::chrono::steady_clock::now();
    uint64_t writeUs = 0;
    if (source) {
        util::logLine("archive_utils: extractLibarchiveInternal stream start, size=" + std::to_string(streamArchiveSize) + " -> " + destinationDir);
    } else {
        util::logLine("archive_utils: extractLibarchiveInternal start: " + archivePath + " -> " + destinationDir);
    }

    uint64_t totalFileSize = streamArchiveSize;
    if (!source) {
        std::error_code ec;
        try {
            if (!std::filesystem::exists(archivePath, ec)) {
                outError = "Файл архива не найден: " + archivePath;
                util::logLine("archive_utils: archive does not exist: " + archivePath);
                return false;
            }
            totalFileSize = std::filesystem::file_size(archivePath, ec);
            makeDirs(destinationDir);
        } catch (const std::exception& e) {
            outError = e.what();
            util::logLine("archive_utils: exception preparing destination: " + std::string(e.what()));
            return false;
        }

        if (knownUncompressedSize == 0 && knownTotalEntries == 0 && singleEntryFilter.empty()) {
            getArchiveTotals(archivePath, knownUncompressedSize, knownTotalEntries);
        }
    } else {
        makeDirs(destinationDir);
    }

    struct archive* a = archive_read_new();
    if (!a) {
        outError = "Failed to allocate libarchive reader";
        util::logLine("archive_utils: failed to allocate libarchive reader");
        return false;
    }

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    ArchiveStreamReader streamReader;
    int r = ARCHIVE_OK;
    if (source) {
        streamReader.source = source;
        streamReader.totalSize = streamArchiveSize;
        streamReader.currentOffset = 0;
        streamReader.cancelToken = cancelToken;
        streamReader.pauseToken = pauseToken;
        // 1 MB per read: each read is a call into the torrent engine.
        streamReader.buffer.resize(1024 * 1024);

        archive_read_set_callback_data(a, &streamReader);
        archive_read_set_read_callback(a, streamArchiveRead);
        archive_read_set_seek_callback(a, streamArchiveSeek);
        archive_read_set_skip_callback(a, streamArchiveSkip);
        archive_read_set_close_callback(a, streamArchiveClose);
        r = archive_read_open1(a);
    } else {
        r = archive_read_open_filename(a, archivePath.c_str(), 65536);
    }

    if (r != ARCHIVE_OK) {
        const char* err = archive_error_string(a);
        outError = err ? err : "Failed to open archive";
        util::logLine("archive_utils: failed to open archive: " + outError);
        archive_read_free(a);
        return false;
    }
    util::logLine("archive_utils: archive opened successfully, total size=" + std::to_string(totalFileSize) +
                  ", uncompressed size=" + std::to_string(knownUncompressedSize) +
                  ", total entries=" + std::to_string(knownTotalEntries));

    ArchiveProgress progress;
    progress.totalArchiveSize = totalFileSize;
    progress.totalUncompressedSize = knownUncompressedSize;
    progress.totalEntries = knownTotalEntries;

    auto updateProgressPercentage = [&]() {
        if (progress.totalUncompressedSize > 0) {
            float pct = static_cast<float>((static_cast<double>(progress.bytesExtracted) * 100.0) / static_cast<double>(progress.totalUncompressedSize));
            progress.percentage = std::clamp(pct, 0.0f, 100.0f);
        } else {
            la_int64_t pos = archive_filter_bytes(a, -1);
            if (pos <= 0) pos = archive_read_header_position(a);
            if (totalFileSize > 0 && pos > 0) {
                float pct = (static_cast<float>(pos) / static_cast<float>(totalFileSize)) * 100.0f;
                if (!std::isnan(pct) && !std::isinf(pct)) {
                    progress.percentage = std::clamp(pct, 0.0f, 100.0f);
                }
            } else if (progress.totalEntries > 0) {
                float pct = static_cast<float>((static_cast<double>(progress.entriesProcessed) * 100.0) / static_cast<double>(progress.totalEntries));
                progress.percentage = std::clamp(pct, 0.0f, 100.0f);
            }
        }
    };

    struct archive_entry* entry = nullptr;
    bool success = true;

    std::string normFilter = sanitizeEntryPath(singleEntryFilter);

    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (r == ARCHIVE_WARN) {
            const char* w = archive_error_string(a);
            if (w && *w) {
                util::logLine("archive_utils: header warning: " + std::string(w));
            }
        }

        if (cancelToken && cancelToken->load()) {
            outError = "Распаковка отменена пользователем";
            util::logLine("archive_utils: extraction cancelled by user");
            success = false;
            break;
        }
        if (pauseToken && pauseToken->load()) {
            while (pauseToken->load() && (!cancelToken || !cancelToken->load()) && !g_appExiting.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (cancelToken && cancelToken->load()) {
                outError = "Распаковка отменена пользователем";
                success = false;
                break;
            }
        }

        const char* currentEntryName = archive_entry_pathname_utf8(entry);
        if (!currentEntryName) currentEntryName = archive_entry_pathname(entry);
        if (!currentEntryName || !*currentEntryName) {
            archive_read_data_skip(a);
            continue;
        }

        std::string cleanName = sanitizeEntryPath(currentEntryName);
        if (cleanName.empty()) {
            archive_read_data_skip(a);
            continue;
        }

        bool isDir = (archive_entry_filetype(entry) == AE_IFDIR) ||
                     ((archive_entry_mode(entry) & 0170000) == 0040000) ||
                     (!cleanName.empty() && (cleanName.back() == '/' || cleanName.back() == '\\'));

        if (!normFilter.empty()) {
            if (cleanName != normFilter && cleanName.rfind(normFilter + "/", 0) != 0) {
                archive_read_data_skip(a);
                continue;
            }
            if (progress.totalUncompressedSize == 0 && !isDir) {
                la_int64_t esz = archive_entry_size(entry);
                if (esz > 0) {
                    progress.totalUncompressedSize = static_cast<uint64_t>(esz);
                }
                progress.totalEntries = 1;
            }
        }

        std::string targetPath = cleanName;
        if (pathTransform) {
            targetPath = pathTransform(cleanName, isDir);
            if (targetPath.empty()) {
                archive_read_data_skip(a);
                continue;
            }
        }

        progress.currentFileName = cleanName;
        progress.entriesProcessed++;

        if (progress.entriesProcessed == 1 || (progress.entriesProcessed % 50 == 0)) {
            util::logLine("archive_utils: extracting entry #" + std::to_string(progress.entriesProcessed) + ": " + cleanName);
        }

        std::string fullPath;
#ifdef _WIN32
        bool isAbs = (targetPath.size() >= 2 && targetPath[1] == ':') ||
                     (targetPath.rfind("./", 0) == 0) ||
                     (targetPath.rfind("sdmc:/", 0) == 0);
#else
        bool isAbs = (targetPath.rfind("sdmc:/", 0) == 0) ||
                     (!targetPath.empty() && targetPath[0] == '/');
#endif
        if (isAbs || destinationDir.empty()) {
            fullPath = targetPath;
        } else {
            fullPath = destinationDir;
            if (!fullPath.empty() && fullPath.back() != '/') {
                fullPath += "/";
            }
            fullPath += targetPath;
        }

        if (isDir) {
            makeDirs(fullPath);
        } else {
            size_t lastSlash = fullPath.rfind('/');
            if (lastSlash != std::string::npos) {
                makeDirs(fullPath.substr(0, lastSlash));
            }
            if (journal) journal->noteFile(fullPath);

            FILE* outFile = fopen(fullPath.c_str(), "wb");
            if (!outFile) {
                std::remove(fullPath.c_str());
                outFile = fopen(fullPath.c_str(), "wb");
            }

            if (!outFile) {
                outError = "Не удалось создать файл: " + fullPath;
                util::logLine("archive_utils: fopen failed: " + fullPath);
                success = false;
                break;
            }

            // Large write buffer: libarchive hands out small blocks, and writing each
            // of them straight to the SD card made extraction slower than the download.
            static thread_local std::vector<char> writeBuf;
            if (writeBuf.size() < kExtractWriteBuffer) writeBuf.resize(kExtractWriteBuffer);
            std::setvbuf(outFile, writeBuf.data(), _IOFBF, writeBuf.size());

            const void* buff = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            la_int64_t filePos = 0;   // where the next fwrite lands
            uint64_t entryBytesExtracted = 0;

            while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
                if (cancelToken && cancelToken->load()) {
                    outError = "Распаковка отменена пользователем";
                    success = false;
                    break;
                }
                if (pauseToken && pauseToken->load()) {
                    while (pauseToken->load() && (!cancelToken || !cancelToken->load()) && !g_appExiting.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    if (cancelToken && cancelToken->load()) {
                        outError = "Распаковка отменена пользователем";
                        success = false;
                        break;
                    }
                }

                if (size > 0 && buff != nullptr) {
                    // Seek only for sparse files: a seek flushes the buffer.
                    if (offset != filePos) {
#if defined(_WIN32)
                        _fseeki64(outFile, offset, SEEK_SET);
#else
                        fseeko(outFile, static_cast<off_t>(offset), SEEK_SET);
#endif
                        filePos = offset;
                    }
                    const auto writeStart = std::chrono::steady_clock::now();
                    size_t written = fwrite(buff, 1, size, outFile);
                    writeUs += elapsedUs(writeStart);
                    filePos += static_cast<la_int64_t>(written);
                    if (written != size) {
                        outError = "Ошибка записи на диск: " + fullPath;
                        util::logLine("archive_utils: fwrite failed for " + fullPath);
                        success = false;
                        break;
                    }
                    progress.bytesExtracted += size;
                    entryBytesExtracted += size;
                }

                updateProgressPercentage();

                if (progressCb) {
                    progressCb(progress);
                }
            }

            {
                const auto closeStart = std::chrono::steady_clock::now();
                fclose(outFile);   // flushes the rest of the buffer
                writeUs += elapsedUs(closeStart);
            }

            if (!success) {
                if (cancelToken && cancelToken->load()) {
                    std::remove(fullPath.c_str());
                }
                break;
            }

            if (r != ARCHIVE_EOF && r < ARCHIVE_OK) {
                const char* err = archive_error_string(a);
                std::string errStr = err ? err : "Ошибка чтения блока данных архива";
                if (errStr.find("CRC") != std::string::npos && entryBytesExtracted > 0) {
                    util::logLine("archive_utils: warning: " + errStr + " for " + cleanName +
                                  " (known libarchive RAR issue). Data extracted: " +
                                  std::to_string(entryBytesExtracted) + " bytes. Continuing extraction.");
                } else {
                    outError = errStr;
                    util::logLine("archive_utils: archive_read_data_block error: " + outError +
                                  " while extracting " + cleanName + " (code=" + std::to_string(r) + ")");
                    success = false;
                    break;
                }
            }
        }

        updateProgressPercentage();
        if (progressCb) {
            progressCb(progress);
        }
    }

    if (r < ARCHIVE_OK && r != ARCHIVE_EOF && success) {
        const char* err = archive_error_string(a);
        outError = err ? err : "Ошибка чтения архива";
        util::logLine("archive_utils: archive header error: " + outError);
        success = false;
    }

    if (success && outError.empty()) {
        progress.percentage = 100.0f;
        if (progress.totalUncompressedSize > 0) {
            progress.bytesExtracted = progress.totalUncompressedSize;
        }
        if (progress.totalEntries > 0) {
            progress.entriesProcessed = progress.totalEntries;
        }
        if (progressCb) {
            progressCb(progress);
        }
    }

    archive_read_close(a);
    archive_read_free(a);

    util::logLine("archive_utils: extractLibarchiveInternal finished, success=" + std::to_string(success));
    {
        const double totalS = elapsedUs(extractStart) / 1e6;
        const double readS = streamReader.readUs / 1e6;
        const double writeS = writeUs / 1e6;
        const double unpackS = std::max(0.0, totalS - readS - writeS);
        const double outMB = progress.bytesExtracted / 1048576.0;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "archive_utils: timing total=%.1fs wait_data=%.1fs write=%.1fs unpack=%.1fs out=%.0fMB "
                      "(%.1f MB/s overall, %.1f MB/s without waiting)",
                      totalS, readS, writeS, unpackS, outMB, totalS > 0 ? outMB / totalS : 0.0,
                      (totalS - readS) > 0 ? outMB / (totalS - readS) : 0.0);
        util::logLine(buf);
    }
    return success;
}

bool extractArchive(
    const std::string& archivePath,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError,
    PathTransformFn pathTransform
) {
    ScopedCpuBoost boostGuard; // raises CPU to 1785 MHz and prevents auto-sleep!
    ExtractionJournal journal;  // removes what this extraction created if it fails

    // 7z archives try 7-Zip SDK decoder first (supports all ARM64/ARM/x86 BCJ, BCJ2, PPMd codecs)
    if (is7zFile(archivePath)) {
        uint64_t knownUncompressed = 0;
        size_t knownEntries = 0;
        bool noFallback = false;
        bool ok = extract7zArchive(archivePath, destinationDir, progressCb, cancelToken, outError,
                                   &knownUncompressed, &knownEntries, pathTransform, &journal, &noFallback);
        if (ok) return true;
        if (noFallback || (cancelToken && cancelToken->load())) {
            journal.rollback();
            return false;
        }

        // If 7zsdk indicated requires_streaming (e.g. folder > 64 MB / 2 GB file) or memory error,
        // seamlessly fall back to streaming libarchive which streams 64KB blocks directly to disk!
        util::logLine("archive_utils: 7zsdk returned '" + outError + "', falling back to streaming libarchive");
        std::string fallbackErr;
        if (extractLibarchiveInternal(archivePath, nullptr, 0, destinationDir, "", progressCb, cancelToken, nullptr, fallbackErr, knownUncompressed, knownEntries, pathTransform, &journal)) {
            util::logLine("archive_utils: streaming libarchive fallback succeeded for 7z archive!");
            outError.clear();
            return true;
        }
        if (outError.empty() || outError == k7zRequiresStreaming) {
            outError = fallbackErr.empty() ? "Ошибка распаковки 7z архива" : fallbackErr;
        }
        journal.rollback();
        return false;
    }

    bool ok = extractLibarchiveInternal(archivePath, nullptr, 0, destinationDir, "", progressCb, cancelToken, nullptr, outError, 0, 0, pathTransform, &journal);
    if (!ok) journal.rollback();
    return ok;
}

bool extractArchiveFromSource(
    datasource::IDataSource* source,
    uint64_t archiveSize,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::shared_ptr<std::atomic<bool>> pauseToken,
    std::string& outError,
    PathTransformFn pathTransform
) {
    if (!source) {
        outError = "Null data source";
        return false;
    }

    ScopedCpuBoost boostGuard; // raises CPU to 1785 MHz and prevents auto-sleep!
    ExtractionJournal journal;  // removes what this extraction created if it fails

    // The 7z header (start + tail of the stream) is checked first; an archive no
    // decoder can handle is rejected before the bulk of the torrent is fetched.
    uint64_t knownUncompressed = 0;
    size_t knownEntries = 0;
    bool noFallback = false;
    bool ok = extract7zArchiveFromSource(source, archiveSize, destinationDir, progressCb,
                                         cancelToken, pauseToken, outError,
                                         &knownUncompressed, &knownEntries, pathTransform,
                                         &journal, &noFallback);
    if (ok) return true;
    if (noFallback || (cancelToken && cancelToken->load())) {
        journal.rollback();
        return false;
    }

    util::logLine("archive_utils: extractArchiveFromSource 7zsdk returned '" + outError + "', falling back to streaming libarchive");
    std::string fallbackErr;
    if (extractLibarchiveInternal("", source, archiveSize, destinationDir, "", progressCb, cancelToken, pauseToken, fallbackErr, knownUncompressed, knownEntries, pathTransform, &journal)) {
        util::logLine("archive_utils: streaming libarchive fallback succeeded for stream archive!");
        outError.clear();
        return true;
    }
    if (outError.empty() || outError == k7zRequiresStreaming) {
        outError = fallbackErr.empty() ? "Ошибка распаковки потокового архива" : fallbackErr;
    }
    journal.rollback();
    return false;
}

bool extractSingleFileFromArchive(
    const std::string& archivePath,
    const std::string& innerFilePath,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError
) {
    ScopedCpuBoost boostGuard;
    ExtractionJournal journal;
    bool ok = extractLibarchiveInternal(archivePath, nullptr, 0, destinationDir, innerFilePath, progressCb, cancelToken, nullptr, outError, 0, 0, nullptr, &journal);
    if (!ok) journal.rollback();
    return ok;
}

bool createZipArchive(
    const std::string& archivePath,
    const std::vector<std::string>& sourcePaths,
    const std::string& baseDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError,
    bool store_only
) {
    ScopedCpuBoost boostGuard;
    if (sourcePaths.empty()) {
        outError = "No source files specified";
        return false;
    }

    util::logLine("archive_utils: createZipArchive start: " + archivePath + ", sources count=" + std::to_string(sourcePaths.size()));

    struct ItemToArchive {
        std::string fullPath;
        std::string relPath;
        bool isDir = false;
        uint64_t size = 0;
        time_t mtime = 0;
    };

    std::vector<ItemToArchive> items;
    uint64_t totalBytes = 0;
    std::error_code ec;

    std::string cleanBase = normalizeFsPath(baseDir);
    while (cleanBase.size() > 1 && cleanBase.back() == '/' &&
           !(cleanBase.size() >= 2 && cleanBase[cleanBase.size() - 2] == ':')) {
        cleanBase.pop_back();
    }
    std::string cleanArchivePath = normalizeFsPath(archivePath);
    std::string cleanTmpPath = normalizeFsPath(archivePath + ".tsnx_tmp");

    auto collectItem = [&](const std::filesystem::path& p) {
        std::string cleanP = normalizeFsPath(p.generic_string());
        while (cleanP.size() > 1 && cleanP.back() == '/' &&
               !(cleanP.size() >= 2 && cleanP[cleanP.size() - 2] == ':')) {
            cleanP.pop_back();
        }

        // Avoid archiving the target archive itself or its temporary file
        // (do NOT use std::filesystem::equivalent on Switch sdmc:/ as st_ino is 0 for all files)
        if (cleanP == cleanTmpPath || cleanP == cleanArchivePath) {
            return;
        }

        std::string rel;
        if (!cleanBase.empty() && cleanP.size() > cleanBase.size() &&
            cleanP.compare(0, cleanBase.size(), cleanBase) == 0) {
            if (cleanBase.back() == '/') {
                rel = cleanP.substr(cleanBase.size());
            } else if (cleanP[cleanBase.size()] == '/') {
                rel = cleanP.substr(cleanBase.size() + 1);
            }
        }
        while (!rel.empty() && rel.front() == '/') {
            rel.erase(rel.begin());
        }
        if (rel.empty() || rel == ".") {
            rel = p.filename().generic_string();
        }

        bool isDir = std::filesystem::is_directory(p, ec);
        uint64_t sz = 0;
        if (!isDir && std::filesystem::is_regular_file(p, ec)) {
            sz = std::filesystem::file_size(p, ec);
        }

        time_t mt = 0;
        auto ftime = std::filesystem::last_write_time(p, ec);
        if (!ec) {
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            mt = std::chrono::system_clock::to_time_t(sctp);
        } else {
            mt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        }

        ItemToArchive item;
        item.fullPath = cleanP;
        item.relPath = rel;
        item.isDir = isDir;
        item.size = sz;
        item.mtime = mt;
        items.push_back(std::move(item));
        if (!isDir) {
            totalBytes += sz;
        }
    };

    for (const auto& src : sourcePaths) {
        if (cancelToken && cancelToken->load()) {
            outError = "Cancelled";
            return false;
        }
        std::string cleanSrc = normalizeFsPath(src);
        std::filesystem::path srcP(cleanSrc);
        if (!std::filesystem::exists(srcP, ec)) continue;

        if (std::filesystem::is_directory(srcP, ec)) {
            collectItem(srcP);
            try {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(srcP, std::filesystem::directory_options::skip_permission_denied, ec)) {
                    if (cancelToken && cancelToken->load()) {
                        outError = "Cancelled";
                        return false;
                    }
                    collectItem(entry.path());
                }
            } catch (...) {}
        } else {
            collectItem(srcP);
        }
    }

    if (items.empty()) {
        outError = "No valid files found to archive";
        util::logLine("archive_utils: createZipArchive failed: " + outError);
        return false;
    }

    util::logLine("archive_utils: createZipArchive collected " + std::to_string(items.size()) +
                  " items, totalBytes=" + std::to_string(totalBytes));

    try {
        std::filesystem::path outP(cleanArchivePath);
        if (outP.has_parent_path()) {
            safeCreateDirectories(outP.parent_path().generic_string());
        }
    } catch (...) {}

    std::string tmpArchivePath = cleanTmpPath;
    std::filesystem::remove(tmpArchivePath, ec);

    struct archive* a = archive_write_new();
    if (!a) {
        outError = "Failed to allocate libarchive writer";
        util::logLine("archive_utils: " + outError);
        return false;
    }

    archive_write_set_format_zip(a);
    if (store_only) {
        archive_write_zip_set_compression_store(a);
    } else {
        archive_write_zip_set_compression_deflate(a);
    }
    // Over 4 GB on a FAT32 card the file must be a Horizon "big file".
    prepareBigFile(tmpArchivePath, totalBytes);

    int r = archive_write_open_filename(a, tmpArchivePath.c_str());
    if (r != ARCHIVE_OK) {
        const char* err = archive_error_string(a);
        outError = err ? err : "Failed to open output archive file";
        util::logLine("archive_utils: failed to open output archive: " + outError);
        archive_write_free(a);
        return false;
    }

    ArchiveProgress progress;
    progress.totalEntries = items.size();
    progress.totalUncompressedSize = totalBytes;
    progress.entriesProcessed = 0;
    progress.bytesExtracted = 0;
    progress.percentage = 0.0f;
    progress.currentFileName = items.front().relPath;
    if (progressCb) {
        progressCb(progress);
    }

    std::vector<char> buffer(256 * 1024);
    bool success = true;

    for (const auto& it : items) {
        if (cancelToken && cancelToken->load()) {
            util::logLine("archive_utils: createZipArchive cancelled by user");
            outError = "Cancelled";
            success = false;
            break;
        }

        progress.currentFileName = it.relPath;
        if (progressCb) {
            progressCb(progress);
        }

        struct archive_entry* entry = archive_entry_new();
        if (!entry) {
            outError = "Failed to create archive entry";
            success = false;
            break;
        }

        archive_entry_set_pathname(entry, it.relPath.c_str());
        if (it.mtime > 0) {
            archive_entry_set_mtime(entry, it.mtime, 0);
        }

        if (it.isDir) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0755);
            archive_entry_set_size(entry, 0);
            int hr = archive_write_header(a, entry);
            archive_entry_free(entry);
            if (hr < ARCHIVE_OK) {
                const char* err = archive_error_string(a);
                outError = err ? err : "Failed to write directory entry header";
                success = false;
                break;
            }
            archive_write_finish_entry(a);
            progress.entriesProcessed++;
        } else {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_entry_set_size(entry, static_cast<la_int64_t>(it.size));
            int hr = archive_write_header(a, entry);
            archive_entry_free(entry);
            if (hr < ARCHIVE_OK) {
                const char* err = archive_error_string(a);
                outError = err ? err : "Failed to write file entry header";
                success = false;
                break;
            }

            FILE* fp = std::fopen(it.fullPath.c_str(), "rb");
            if (!fp) {
                util::logLine("archive_utils: warning: could not open file " + it.fullPath);
            } else {
                while (true) {
                    if (cancelToken && cancelToken->load()) {
                        outError = "Cancelled";
                        success = false;
                        break;
                    }
                    size_t count = std::fread(buffer.data(), 1, buffer.size(), fp);
                    if (count > 0) {
                        la_ssize_t written = archive_write_data(a, buffer.data(), count);
                        if (written < 0) {
                            const char* err = archive_error_string(a);
                            outError = err ? err : "Error writing archive data";
                            success = false;
                            break;
                        }
                        progress.bytesExtracted += static_cast<uint64_t>(count);
                        if (totalBytes > 0) {
                            float pct = (static_cast<float>(progress.bytesExtracted) / static_cast<float>(totalBytes)) * 100.0f;
                            if (!std::isnan(pct) && !std::isinf(pct)) {
                                progress.percentage = std::clamp(pct, 0.0f, 99.9f);
                            }
                        } else if (progress.totalEntries > 0) {
                            float pct = (static_cast<float>(progress.entriesProcessed) / static_cast<float>(progress.totalEntries)) * 100.0f;
                            progress.percentage = std::clamp(pct, 0.0f, 99.9f);
                        }
                        if (progressCb) {
                            progressCb(progress);
                        }
                    }
                    if (count < buffer.size()) {
                        if (std::ferror(fp)) {
                            outError = "Error reading source file: " + it.fullPath;
                            success = false;
                        }
                        break;
                    }
                }
                std::fclose(fp);
            }

            if (!success) {
                break;
            }

            archive_write_finish_entry(a);
            progress.entriesProcessed++;
        }
    }

    archive_write_close(a);
    archive_write_free(a);

    if (!success || (cancelToken && cancelToken->load())) {
        std::filesystem::remove(tmpArchivePath, ec);
        return false;
    }

    std::string moveErr;
    std::filesystem::remove(archivePath, ec);
    if (!util::movePath(tmpArchivePath, archivePath, moveErr)) {
        outError = "Failed to finalize archive: " + moveErr;
        std::filesystem::remove(tmpArchivePath, ec);
        return false;
    }

    progress.percentage = 100.0f;
    if (progressCb) {
        progressCb(progress);
    }

    util::logLine("archive_utils: createZipArchive completed successfully: " + archivePath);
    return true;
}

} // namespace util
