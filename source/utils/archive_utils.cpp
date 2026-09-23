#include "archive_utils.h"
#include "file_ops.h"
#include "log.h"
#include "sevenzip_utils.h"
#include "switch_utils.h"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

namespace util {

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
           endsWith(".xz");
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

static bool extractLibarchiveInternal(
    const std::string& archivePath,
    const std::string& destinationDir,
    const std::string& singleEntryFilter,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError,
    uint64_t knownUncompressedSize = 0,
    size_t knownTotalEntries = 0
) {
    util::logLine("archive_utils: extractLibarchiveInternal start: " + archivePath + " -> " + destinationDir);

    std::error_code ec;
    uint64_t totalFileSize = 0;
    try {
        if (!std::filesystem::exists(archivePath, ec)) {
            outError = "Файл архива не найден: " + archivePath;
            util::logLine("archive_utils: archive does not exist: " + archivePath);
            return false;
        }
        totalFileSize = std::filesystem::file_size(archivePath, ec);
        safeCreateDirectories(destinationDir);
    } catch (const std::exception& e) {
        outError = e.what();
        util::logLine("archive_utils: exception preparing destination: " + std::string(e.what()));
        return false;
    }

    if (knownUncompressedSize == 0 && knownTotalEntries == 0 && singleEntryFilter.empty()) {
        getArchiveTotals(archivePath, knownUncompressedSize, knownTotalEntries);
    }

    struct archive* a = archive_read_new();
    if (!a) {
        outError = "Failed to allocate libarchive reader";
        util::logLine("archive_utils: failed to allocate libarchive reader");
        return false;
    }

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    int r = archive_read_open_filename(a, archivePath.c_str(), 65536);
    if (r != ARCHIVE_OK) {
        const char* err = archive_error_string(a);
        outError = err ? err : "Failed to open archive file";
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

        progress.currentFileName = cleanName;
        progress.entriesProcessed++;

        if (progress.entriesProcessed == 1 || (progress.entriesProcessed % 50 == 0)) {
            util::logLine("archive_utils: extracting entry #" + std::to_string(progress.entriesProcessed) + ": " + cleanName);
        }

        std::string fullPath = destinationDir;
        if (!fullPath.empty() && fullPath.back() != '/') {
            fullPath += "/";
        }
        fullPath += cleanName;

        if (isDir) {
            safeCreateDirectories(fullPath);
        } else {
            size_t lastSlash = fullPath.rfind('/');
            if (lastSlash != std::string::npos) {
                safeCreateDirectories(fullPath.substr(0, lastSlash));
            }

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

            const void* buff = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            uint64_t entryBytesExtracted = 0;

            while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
                if (cancelToken && cancelToken->load()) {
                    outError = "Распаковка отменена пользователем";
                    success = false;
                    break;
                }

                if (size > 0 && buff != nullptr) {
#if defined(_WIN32)
                    _fseeki64(outFile, offset, SEEK_SET);
#else
                    fseeko(outFile, static_cast<off_t>(offset), SEEK_SET);
#endif
                    size_t written = fwrite(buff, 1, size, outFile);
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

            fclose(outFile);

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
                                  " for entry=" + cleanName +
                                  " format=" + std::string(archive_format_name(a) ? archive_format_name(a) : "unknown") +
                                  " code=" + std::to_string(archive_errno(a)));
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

    if (success && r != ARCHIVE_EOF && r < ARCHIVE_WARN) {
        const char* err = archive_error_string(a);
        outError = err ? err : "Ошибка чтения заголовка архива";
        util::logLine("archive_utils: archive header error: " + outError);
        success = false;
    }

    if (success) {
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
    return success;
}

bool extractArchive(
    const std::string& archivePath,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError
) {
    ScopedCpuBoost boostGuard; // raises CPU to 1785 MHz and prevents auto-sleep!

    // 7z archives try 7-Zip SDK decoder first (supports all ARM64/ARM/x86 BCJ, BCJ2, PPMd codecs)
    if (is7zFile(archivePath)) {
        uint64_t knownUncompressed = 0;
        size_t knownEntries = 0;
        bool ok = extract7zArchive(archivePath, destinationDir, progressCb, cancelToken, outError, &knownUncompressed, &knownEntries);
        if (ok) return true;
        if (cancelToken && cancelToken->load()) return false;

        // If 7zsdk indicated requires_streaming (e.g. folder > 64 MB / 2 GB file) or memory error,
        // seamlessly fall back to streaming libarchive which streams 64KB blocks directly to disk!
        util::logLine("archive_utils: 7zsdk returned '" + outError + "', falling back to streaming libarchive");
        std::string fallbackErr;
        if (extractLibarchiveInternal(archivePath, destinationDir, "", progressCb, cancelToken, fallbackErr, knownUncompressed, knownEntries)) {
            util::logLine("archive_utils: streaming libarchive fallback succeeded for 7z archive!");
            outError.clear();
            return true;
        }
        if (outError.empty() || outError == "requires_streaming") {
            outError = fallbackErr.empty() ? "Ошибка распаковки 7z архива" : fallbackErr;
        }
        return false;
    }

    return extractLibarchiveInternal(archivePath, destinationDir, "", progressCb, cancelToken, outError, 0, 0);
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
    return extractLibarchiveInternal(archivePath, destinationDir, innerFilePath, progressCb, cancelToken, outError, 0, 0);
}

bool createZipArchive(
    const std::string& archivePath,
    const std::vector<std::string>& sourcePaths,
    const std::string& baseDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError
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

    std::filesystem::path base(baseDir);

    auto collectItem = [&](const std::filesystem::path& p) {
        std::string rel;
        try {
            if (!baseDir.empty() && std::filesystem::exists(base, ec)) {
                rel = std::filesystem::relative(p, base, ec).generic_string();
            }
        } catch (...) {
            rel = "";
        }
        if (rel.empty() || rel == ".") {
            rel = p.filename().generic_string();
        }

        // Avoid archiving the target archive itself or its temporary file
        std::filesystem::path tmpTargetPath(archivePath + ".tsnx_tmp");
        std::filesystem::path realTargetPath(archivePath);
        if (std::filesystem::equivalent(p, tmpTargetPath, ec) || std::filesystem::equivalent(p, realTargetPath, ec)) {
            return;
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
        item.fullPath = p.generic_string();
        item.relPath = rel;
        item.isDir = isDir;
        item.size = sz;
        item.mtime = mt;
        items.push_back(item);
        if (!isDir) {
            totalBytes += sz;
        }
    };

    for (const auto& src : sourcePaths) {
        std::filesystem::path srcP(src);
        if (!std::filesystem::exists(srcP, ec)) continue;

        if (std::filesystem::is_directory(srcP, ec)) {
            collectItem(srcP);
            try {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(srcP, std::filesystem::directory_options::skip_permission_denied, ec)) {
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

    try {
        std::filesystem::path outP(archivePath);
        if (outP.has_parent_path()) {
            safeCreateDirectories(outP.parent_path().generic_string());
        }
    } catch (...) {}

    std::string tmpArchivePath = archivePath + ".tsnx_tmp";
    std::filesystem::remove(tmpArchivePath, ec);

    struct archive* a = archive_write_new();
    if (!a) {
        outError = "Failed to allocate libarchive writer";
        util::logLine("archive_utils: " + outError);
        return false;
    }

    archive_write_set_format_zip(a);
    archive_write_zip_set_compression_deflate(a);

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

    std::vector<char> buffer(128 * 1024);
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

            std::ifstream file(it.fullPath, std::ios::binary);
            if (!file.is_open()) {
                util::logLine("archive_utils: warning: could not open file " + it.fullPath);
            } else {
                while (file) {
                    if (cancelToken && cancelToken->load()) {
                        outError = "Cancelled";
                        success = false;
                        break;
                    }
                    file.read(buffer.data(), buffer.size());
                    std::streamsize count = file.gcount();
                    if (count > 0) {
                        la_ssize_t written = archive_write_data(a, buffer.data(), static_cast<size_t>(count));
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
                        }
                        if (progressCb) {
                            progressCb(progress);
                        }
                    }
                }
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
