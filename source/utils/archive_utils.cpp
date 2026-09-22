#include "archive_utils.h"
#include "file_ops.h"
#include "log.h"
#include "sevenzip_utils.h"
#include "switch_utils.h"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
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
                FileItem item;
                item.name = clean;
                item.path = archivePath + "/" + clean;
                item.isDir = isDir;
                item.size = fileSize;
                item.modifiedTime = mtime;
                outItems.push_back(item);
            } else {
                std::string top = clean.substr(0, slash);
                if (seenDirs.insert(top).second) {
                    FileItem dirItem;
                    dirItem.name = top;
                    dirItem.path = archivePath + "/" + top;
                    dirItem.isDir = true;
                    dirItem.size = 0;
                    dirItem.modifiedTime = 0;
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
                    FileItem item;
                    item.name = rel;
                    item.path = archivePath + "/" + clean;
                    item.isDir = isDir;
                    item.size = fileSize;
                    item.modifiedTime = mtime;
                    outItems.push_back(item);
                } else {
                    std::string sub = rel.substr(0, slash);
                    if (seenDirs.insert(sub).second) {
                        FileItem dirItem;
                        dirItem.name = sub;
                        dirItem.path = archivePath + "/" + normInner + "/" + sub;
                        dirItem.isDir = true;
                        dirItem.size = 0;
                        dirItem.modifiedTime = 0;
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

static bool extractLibarchiveInternal(
    const std::string& archivePath,
    const std::string& destinationDir,
    const std::string& singleEntryFilter,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError
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
    util::logLine("archive_utils: archive opened successfully, total size=" + std::to_string(totalFileSize));

    ArchiveProgress progress;
    progress.totalArchiveSize = totalFileSize;

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

        if (!normFilter.empty()) {
            if (cleanName != normFilter && cleanName.rfind(normFilter + "/", 0) != 0) {
                archive_read_data_skip(a);
                continue;
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

        bool isDir = (archive_entry_filetype(entry) == AE_IFDIR) ||
                     ((archive_entry_mode(entry) & 0170000) == 0040000) ||
                     (!cleanName.empty() && (cleanName.back() == '/' || cleanName.back() == '\\'));

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

                la_int64_t pos = archive_filter_bytes(a, -1);
                if (pos <= 0) pos = archive_read_header_position(a);
                if (totalFileSize > 0 && pos > 0) {
                    float pct = (static_cast<float>(pos) / static_cast<float>(totalFileSize)) * 100.0f;
                    if (!std::isnan(pct) && !std::isinf(pct)) {
                        progress.percentage = std::clamp(pct, 0.0f, 100.0f);
                    }
                }

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

        la_int64_t pos = archive_filter_bytes(a, -1);
        if (pos <= 0) pos = archive_read_header_position(a);
        if (totalFileSize > 0 && pos > 0) {
            float pct = (static_cast<float>(pos) / static_cast<float>(totalFileSize)) * 100.0f;
            if (!std::isnan(pct) && !std::isinf(pct)) {
                progress.percentage = std::clamp(pct, 0.0f, 100.0f);
            }
        }
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
        bool ok = extract7zArchive(archivePath, destinationDir, progressCb, cancelToken, outError);
        if (ok) return true;
        if (cancelToken && cancelToken->load()) return false;

        // If 7zsdk indicated requires_streaming (e.g. folder > 64 MB / 2 GB file) or memory error,
        // seamlessly fall back to streaming libarchive which streams 64KB blocks directly to disk!
        util::logLine("archive_utils: 7zsdk returned '" + outError + "', falling back to streaming libarchive");
        std::string fallbackErr;
        if (extractLibarchiveInternal(archivePath, destinationDir, "", progressCb, cancelToken, fallbackErr)) {
            util::logLine("archive_utils: streaming libarchive fallback succeeded for 7z archive!");
            outError.clear();
            return true;
        }
        if (outError.empty() || outError == "requires_streaming") {
            outError = fallbackErr.empty() ? "Ошибка распаковки 7z архива" : fallbackErr;
        }
        return false;
    }

    return extractLibarchiveInternal(archivePath, destinationDir, "", progressCb, cancelToken, outError);
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
    return extractLibarchiveInternal(archivePath, destinationDir, innerFilePath, progressCb, cancelToken, outError);
}

} // namespace util
