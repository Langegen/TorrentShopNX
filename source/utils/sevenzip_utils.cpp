#include "sevenzip_utils.h"
#include "file_ops.h"
#include "log.h"

#include "../7zsdk/7z.h"
#include "../7zsdk/7zAlloc.h"
#include "../7zsdk/7zCrc.h"
#include "../7zsdk/7zFile.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace util {

static const size_t kInputBufSize = (size_t)1 << 20;

static void* sdkAlloc(ISzAllocPtr /*p*/, size_t size) { return SzAlloc(nullptr, size); }
static void sdkFree(ISzAllocPtr /*p*/, void* addr) { SzFree(nullptr, addr); }

static const ISzAlloc g_allocImp = { sdkAlloc, sdkFree };
static const ISzAlloc g_allocTempImp = { SzAllocTemp, SzFreeTemp };

bool is7zFile(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 3);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".7z";
}

// Converts UTF-16LE (as produced by SzArEx_GetFileNameUtf16) to UTF-8.
static std::string utf16leToUtf8(const UInt16* s, size_t units) {
    std::string out;
    out.reserve(units);
    for (size_t i = 0; i < units; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units &&
            s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Same path hardening rules as the libarchive extraction path.
static std::string sanitizeEntryName(const std::string& raw) {
    std::string clean = raw;
    while (!clean.empty() && (clean.front() == '/' || clean.front() == '\\')) {
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

static const char* sresToMessage(SRes res) {
    switch (res) {
    case SZ_OK:                 return "";
    case SZ_ERROR_MEM:          return "Не хватает памяти";
    case SZ_ERROR_NO_ARCHIVE:   return "Файл не является архивом 7z";
    case SZ_ERROR_ARCHIVE:      return "Архив повреждён или имеет неверный формат";
    case SZ_ERROR_CRC:          return "Ошибка контрольной суммы в заголовке архива";
    case SZ_ERROR_UNSUPPORTED:  return "Архив использует неподдерживаемые возможности (например, шифрование)";
    case SZ_ERROR_INPUT_EOF:    return "Архив повреждён (неожиданный конец файла)";
    case SZ_ERROR_FAIL:         return "Не удалось прочитать архив";
    case SZ_ERROR_DATA:         return "Ошибка данных в архиве";
    default:                    return "Неизвестная ошибка декодирования 7z";
    }
}

bool extract7zArchive(
    const std::string& archivePath,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError
) {
    util::logLine("sevenzip: extractArchive start: " + archivePath + " -> " + destinationDir);

    std::error_code ec;
    uint64_t totalFileSize = 0;
    try {
        if (!std::filesystem::exists(archivePath, ec)) {
            outError = "Файл архива не найден: " + archivePath;
            util::logLine("sevenzip: archive does not exist: " + archivePath);
            return false;
        }
        totalFileSize = std::filesystem::file_size(archivePath, ec);
        safeCreateDirectories(destinationDir);
    } catch (const std::exception& e) {
        outError = e.what();
        util::logLine("sevenzip: exception preparing destination: " + std::string(e.what()));
        return false;
    }

    CFileInStream archiveStream;
    CLookToRead2 lookStream;
    CSzArEx db;
    SRes res;

    File_Construct(&archiveStream.file);
    archiveStream.wres = 0;

    {
        WRes wres;
#ifdef _WIN32
        std::wstring wpath = std::filesystem::u8path(archivePath).native();
        wres = InFile_OpenW(&archiveStream.file, wpath.c_str());
#else
        wres = InFile_Open(&archiveStream.file, archivePath.c_str());
#endif
        if (wres != 0) {
            outError = "Не удалось открыть архив для чтения";
            util::logLine("sevenzip: InFile_Open failed, wres=" + std::to_string((int)wres));
            return false;
        }
    }

    FileInStream_CreateVTable(&archiveStream);
    archiveStream.wres = 0;

    LookToRead2_CreateVTable(&lookStream, False);
    lookStream.buf = nullptr;
    lookStream.bufSize = 0;
    lookStream.realStream = nullptr;

    CrcGenerateTable();
    SzArEx_Init(&db);

    ArchiveProgress progress;
    progress.totalArchiveSize = totalFileSize;

    bool ok = false;
    UInt32 blockIndex = 0xFFFFFFFF;
    Byte* outBuffer = nullptr;
    size_t outBufferSize = 0;
    UInt16* nameBuf = nullptr;
    size_t nameBufSize = 0;

    res = SZ_OK;
    lookStream.buf = static_cast<Byte*>(SzAlloc(nullptr, kInputBufSize));
    if (!lookStream.buf) {
        res = SZ_ERROR_MEM;
    } else {
        lookStream.bufSize = kInputBufSize;
        lookStream.realStream = &archiveStream.vt;
        LookToRead2_INIT(&lookStream);
    }

    if (res == SZ_OK) {
        res = SzArEx_Open(&db, &lookStream.vt, (ISzAllocPtr)&g_allocImp, (ISzAllocPtr)&g_allocTempImp);
    }

    if (res != SZ_OK) {
        outError = sresToMessage(res);
        util::logLine("sevenzip: SzArEx_Open failed: " + std::to_string((int)res) + " (" + outError + ")");
    } else {
        util::logLine("sevenzip: archive opened, entries=" + std::to_string(db.NumFiles) +
                      " folders=" + std::to_string(db.db.NumFolders));

        UInt64 totalUncompressed = 0;
        for (UInt32 fi = 0; fi < db.NumFiles; fi++) {
            if (!SzArEx_IsDir(&db, fi)) {
                totalUncompressed += SzArEx_GetFileSize(&db, fi);
            }
        }
        progress.totalUncompressedSize = totalUncompressed;
        progress.totalEntries = db.NumFiles;

        auto updateProgressPct = [&]() {
            if (totalUncompressed > 0) {
                float pct = static_cast<float>((static_cast<double>(progress.bytesExtracted) * 100.0) / static_cast<double>(totalUncompressed));
                progress.percentage = std::clamp(pct, 0.0f, 100.0f);
            } else if (db.NumFiles > 0) {
                float pct = static_cast<float>((static_cast<double>(progress.entriesProcessed) * 100.0) / static_cast<double>(db.NumFiles));
                progress.percentage = std::clamp(pct, 0.0f, 100.0f);
            }
        };

        for (UInt32 i = 0; i < db.NumFiles; i++) {
            if (cancelToken && cancelToken->load()) {
                outError = "Распаковка отменена пользователем";
                util::logLine("sevenzip: extraction cancelled by user");
                break;
            }

            size_t nameLen = SzArEx_GetFileNameUtf16(&db, i, nullptr);
            if (nameLen == 0 || nameLen > (1u << 20)) {
                outError = "Некорректное имя файла в архиве";
                util::logLine("sevenzip: invalid entry name length=" + std::to_string(nameLen));
                break;
            }
            if (nameLen > nameBufSize) {
                SzFree(nullptr, nameBuf);
                nameBufSize = nameLen;
                nameBuf = static_cast<UInt16*>(SzAlloc(nullptr, nameBufSize * sizeof(UInt16)));
                if (!nameBuf) {
                    outError = "Не хватает памяти";
                    break;
                }
            }
            SzArEx_GetFileNameUtf16(&db, i, nameBuf);

            const bool isDir = SzArEx_IsDir(&db, i) != 0;
            std::string cleanName = sanitizeEntryName(utf16leToUtf8(nameBuf, nameLen - 1));
            if (cleanName.empty()) {
                continue;
            }

            progress.currentFileName = cleanName;
            progress.entriesProcessed++;

            std::string fullPath = destinationDir;
            if (!fullPath.empty() && fullPath.back() != '/') {
                fullPath += "/";
            }
            fullPath += cleanName;

            if (progress.entriesProcessed == 1 || (progress.entriesProcessed % 50 == 0)) {
                util::logLine("sevenzip: extracting entry #" + std::to_string(progress.entriesProcessed) + ": " + cleanName);
            }

            if (isDir) {
                safeCreateDirectories(fullPath);
                updateProgressPct();
                if (progressCb) progressCb(progress);
                continue;
            }

            size_t offset = 0;
            size_t outSizeProcessed = 0;
            res = SzArEx_Extract(&db, &lookStream.vt, i,
                                 &blockIndex, &outBuffer, &outBufferSize,
                                 &offset, &outSizeProcessed,
                                 (ISzAllocPtr)&g_allocImp, (ISzAllocPtr)&g_allocTempImp);
            if (res != SZ_OK) {
                outError = sresToMessage(res);
                util::logLine("sevenzip: SzArEx_Extract failed for " + cleanName +
                              ": " + std::to_string((int)res) + " (" + outError + ")");
                break;
            }

            // Verify file CRC when the archive stores one.
            if (SzBitWithVals_Check(&db.CRCs, i)) {
                UInt32 crc = CrcUpdate(CRC_INIT_VAL, outBuffer + offset, outSizeProcessed);
                if (CRC_GET_DIGEST(crc) != db.CRCs.Vals[i]) {
                    outError = "Ошибка контрольной суммы: " + cleanName;
                    util::logLine("sevenzip: CRC mismatch for " + cleanName);
                    break;
                }
            }

            size_t lastSlash = fullPath.rfind('/');
            if (lastSlash != std::string::npos) {
                safeCreateDirectories(fullPath.substr(0, lastSlash));
            }

            FILE* outFile = nullptr;
#ifdef _WIN32
            std::wstring wfull = std::filesystem::u8path(fullPath).native();
            outFile = _wfopen(wfull.c_str(), L"wb");
            if (!outFile) {
                _wremove(wfull.c_str());
                outFile = _wfopen(wfull.c_str(), L"wb");
            }
#else
            outFile = fopen(fullPath.c_str(), "wb");
            if (!outFile) {
                std::remove(fullPath.c_str());
                outFile = fopen(fullPath.c_str(), "wb");
            }
#endif
            if (!outFile) {
                outError = "Не удалось создать файл: " + fullPath;
                util::logLine("sevenzip: fopen failed: " + fullPath);
                break;
            }

            size_t written = 0;
            if (outSizeProcessed > 0) {
                written = fwrite(outBuffer + offset, 1, outSizeProcessed, outFile);
            }
            fclose(outFile);

            if (written != outSizeProcessed) {
                outError = "Ошибка записи на диск: " + fullPath;
                util::logLine("sevenzip: fwrite failed for " + fullPath);
                break;
            }

            progress.bytesExtracted += outSizeProcessed;
            updateProgressPct();
            if (progressCb) progressCb(progress);
        }

        if (res == SZ_OK && outError.empty()) {
            progress.percentage = 100.0f;
            if (totalUncompressed > 0) {
                progress.bytesExtracted = totalUncompressed;
            }
            progress.entriesProcessed = db.NumFiles;
            if (progressCb) progressCb(progress);
            ok = true;
        } else if (outError.empty()) {
            // cancelled or broke without setting a message yet
            outError = res == SZ_OK ? "Распаковка прервана" : sresToMessage(res);
        }
    }

    SzFree(nullptr, outBuffer);
    SzFree(nullptr, nameBuf);
    SzFree(nullptr, lookStream.buf);
    lookStream.buf = nullptr;
    SzArEx_Free(&db, (ISzAllocPtr)&g_allocImp);
    File_Close(&archiveStream.file);

    util::logLine("sevenzip: extractArchive finished, success=" + std::string(ok ? "1" : "0"));
    return ok;
}

} // namespace util
