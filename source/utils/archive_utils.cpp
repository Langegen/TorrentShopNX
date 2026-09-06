#include "archive_utils.h"
#include "file_ops.h"
#include "log.h"
#include "sevenzip_utils.h"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cmath>

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


bool extractArchive(
    const std::string& archivePath,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError
) {
    // 7z archives go through the embedded 7-Zip SDK decoder, which supports
    // every codec the official 7-Zip creates (ARM64/ARM/x86 BCJ, BCJ2, PPMd, ...).
    // The devkitPro libarchive build does not, and fails on such archives.
    if (is7zFile(archivePath)) {
        return extract7zArchive(archivePath, destinationDir, progressCb, cancelToken, outError);
    }

    util::logLine("archive_utils: extractArchive start: " + archivePath + " -> " + destinationDir);

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

    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        if (cancelToken && cancelToken->load()) {
            outError = "Распаковка отменена пользователем";
            util::logLine("archive_utils: extraction cancelled by user");
            success = false;
            break;
        }

        const char* currentEntryName = archive_entry_pathname(entry);
        if (!currentEntryName || !*currentEntryName) {
            continue;
        }

        // Sanitize entry name: strip leading slashes and path traversals
        std::string cleanName = currentEntryName;
        while (!cleanName.empty() && (cleanName.front() == '/' || cleanName.front() == '\\')) {
            cleanName.erase(cleanName.begin());
        }
        size_t dotdot;
        while ((dotdot = cleanName.find("..")) != std::string::npos) {
            cleanName.replace(dotdot, 2, "__");
        }
        // Sanitize FAT32 illegal characters: ':', '*', '?', '"', '<', '>', '|'
        for (char& c : cleanName) {
            if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
                c = '_';
            }
        }
        if (cleanName.empty()) {
            continue;
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
                     (!cleanName.empty() && (cleanName.back() == '/' || cleanName.back() == '\\'));

        if (isDir) {
            safeCreateDirectories(fullPath);
        } else {
            // Ensure parent folder exists
            size_t lastSlash = fullPath.rfind('/');
            if (lastSlash != std::string::npos) {
                safeCreateDirectories(fullPath.substr(0, lastSlash));
            }

            FILE* outFile = fopen(fullPath.c_str(), "wb");
            if (!outFile) {
                // If opening failed, try deleting existing file if any and re-try
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
                break;
            }

            if (r != ARCHIVE_EOF && r < ARCHIVE_OK) {
                const char* err = archive_error_string(a);
                outError = err ? err : "Ошибка чтения блока данных архива";
                util::logLine("archive_utils: archive_read_data_block error: " + outError);
                success = false;
                break;
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

    if (success && r != ARCHIVE_EOF && r < ARCHIVE_OK) {
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

    util::logLine("archive_utils: extractArchive finished, success=" + std::to_string(success));
    return success;
}

} // namespace util
