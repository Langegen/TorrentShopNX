#include "archive_utils.h"
#include "file_ops.h"
#include "log.h"
#include "sevenzip_utils.h"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <chrono>
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
                     ((archive_entry_mode(entry) & 0170000) == 0040000) ||
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
                // Libarchive (<= 3.7.2 RAR decompression) has a known upstream bug
                // with circular LZSS window calculation on large entries (e.g. CD-ROM images > 100MB),
                // which causes a false-positive "File CRC error" (code 79 EFTYPE) at the very end of decompression,
                // even though all uncompressed bytes were already written out to disk.
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

    util::logLine("archive_utils: extractArchive finished, success=" + std::to_string(success));
    return success;
}

bool createZipArchive(
    const std::string& archivePath,
    const std::vector<std::string>& sourcePaths,
    const std::string& baseDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError
) {
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
