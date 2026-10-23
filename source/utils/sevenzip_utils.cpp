#include "sevenzip_utils.h"
#include "extraction_journal.h"
#include "file_ops.h"
#include "../datasource/i_data_source.h"
#include <thread>
#include <chrono>
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
#include <unordered_set>

namespace util {

static const size_t kInputBufSize = (size_t)1 << 20;

const char* const k7zRequiresStreaming = "requires_streaming";

// The SDK decodes a whole folder into RAM; bigger folders go to libarchive.
static const UInt64 kSdkMaxFolderSize = 64ULL * 1024 * 1024;

namespace {

// 7z method IDs (7-Zip DOC/Methods.txt).
enum : UInt32 {
    kMethodCopy    = 0,
    kMethodDelta   = 0x03,
    kMethodARM64   = 0x0A,
    kMethodRISCV   = 0x0B,
    kMethodLZMA2   = 0x21,
    kMethodLZMA    = 0x030101,
    kMethodPPMD    = 0x030401,
    kMethodBCJ     = 0x03030103,
    kMethodBCJ2    = 0x0303011B,
    kMethodPPC     = 0x03030205,
    kMethodIA64    = 0x03030401,
    kMethodARM     = 0x03030501,
    kMethodARMT    = 0x03030701,
    kMethodSPARC   = 0x03030805,
    kMethodDeflate = 0x040108,
    kMethodBZip2   = 0x040202,
    kMethodZstd    = 0x04F71101,
};

bool isCryptoMethod(UInt32 id) {
    return (id & 0xFFFF0000u) == 0x06F10000u; // 06F1xxxx: 7zAES, ZipCrypto, RAR AES
}

std::string methodName(UInt32 id) {
    switch (id) {
    case kMethodCopy:    return "Copy";
    case kMethodDelta:   return "Delta";
    case kMethodARM64:   return "ARM64";
    case kMethodRISCV:   return "RISCV";
    case kMethodLZMA2:   return "LZMA2";
    case kMethodLZMA:    return "LZMA";
    case kMethodPPMD:    return "PPMd";
    case kMethodBCJ:     return "BCJ";
    case kMethodBCJ2:    return "BCJ2";
    case kMethodPPC:     return "PPC";
    case kMethodIA64:    return "IA64";
    case kMethodARM:     return "ARM";
    case kMethodARMT:    return "ARMT";
    case kMethodSPARC:   return "SPARC";
    case kMethodDeflate: return "Deflate";
    case kMethodBZip2:   return "BZip2";
    case kMethodZstd:    return "Zstd";
    default: break;
    }
    if (isCryptoMethod(id)) return "AES";
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%X", (unsigned)id);
    return buf;
}

std::string describeChain(const CSzFolder& f) {
    std::string s;
    for (UInt32 i = 0; i < f.NumCoders; i++) {
        if (i) s += "+";
        s += methodName(f.Coders[i].MethodID);
    }
    return s;
}

bool hasCrypto(const CSzFolder& f) {
    for (UInt32 i = 0; i < f.NumCoders; i++) {
        if (isCryptoMethod(f.Coders[i].MethodID)) return true;
    }
    return false;
}

// Mirrors libarchive's 7z reader (init_decompression / setup_decode_folder,
// 3.8.x): coders[0] is the main method, coders[1] an optional filter, or the
// 4-coder BCJ2 layout.
bool libarchiveCanDecode(const CSzFolder& f) {
    auto isMain = [](UInt32 id) {
        return id == kMethodCopy || id == kMethodLZMA || id == kMethodLZMA2 || id == kMethodPPMD ||
               id == kMethodDeflate || id == kMethodBZip2 || id == kMethodZstd;
    };
    const UInt32 main = f.Coders[0].MethodID;
    if (f.NumCoders == 1) return isMain(main);
    if (f.NumCoders == 4) {
        return f.Coders[3].MethodID == kMethodBCJ2 && isMain(main) &&
               isMain(f.Coders[1].MethodID) && isMain(f.Coders[2].MethodID);
    }
    if (f.NumCoders != 2 || !isMain(main)) return false;

    const UInt32 filter = f.Coders[1].MethodID;
    if (main == kMethodLZMA || main == kMethodLZMA2) {
        switch (filter) {
        case kMethodBCJ: case kMethodBCJ2: case kMethodDelta: case kMethodPPC:
        case kMethodIA64: case kMethodARM: case kMethodARMT: case kMethodSPARC:
            return true;
        case kMethodARM64: case kMethodRISCV:
            // Goes through liblzma's filter, compiled in only with a new enough
            // liblzma. RISCV (xz 5.6) is assumed to follow ARM64 (xz 5.4).
            return libarchiveSupports7zArm64Lzma();
        default:
            return false;
        }
    }
    switch (filter) {
    case kMethodBCJ: case kMethodBCJ2: case kMethodARM: case kMethodARM64:
    case kMethodPPC: case kMethodSPARC:
        return true;
    default:
        return false;
    }
}

enum class DecodePlan { Sdk, Libarchive, Unsupported };

// Decides who decodes the archive using only the header, before any payload
// is read. Fills outError for Libarchive/Unsupported.
DecodePlan plan7zDecode(const CSzArEx& db, UInt64 maxFolderSize, std::string& outError) {
    struct ChainInfo { size_t count = 0; bool sdk = true; bool la = true; UInt64 maxSize = 0; };
    std::vector<std::pair<std::string, ChainInfo>> chains; // insertion order, few distinct entries
    bool sdkAll = maxFolderSize <= kSdkMaxFolderSize;
    bool laAll = true;
    bool encrypted = false;

    for (UInt32 fi = 0; fi < db.db.NumFolders; fi++) {
        CSzFolder f;
        const bool parsed = SzAr_GetFolder(&db.db, fi, &f) == SZ_OK && f.NumCoders > 0;
        const std::string chain = parsed ? describeChain(f) : "?";
        const bool sdk = parsed && SzFolder_CheckSupported(&f) == SZ_OK;
        const bool la = parsed && libarchiveCanDecode(f);
        if (parsed && hasCrypto(f)) encrypted = true;
        sdkAll = sdkAll && sdk;
        laAll = laAll && la;

        auto it = std::find_if(chains.begin(), chains.end(),
                               [&](const auto& c) { return c.first == chain; });
        if (it == chains.end()) {
            chains.push_back({chain, ChainInfo{}});
            it = chains.end() - 1;
        }
        it->second.count++;
        it->second.sdk = it->second.sdk && sdk;
        it->second.la = it->second.la && la;
        it->second.maxSize = std::max<UInt64>(it->second.maxSize, SzAr_GetFolderUnpackSize(&db.db, fi));
    }

    std::string summary;
    for (const auto& c : chains) {
        if (!summary.empty()) summary += ", ";
        summary += c.first + " x" + std::to_string(c.second.count) +
                   " (max " + std::to_string(c.second.maxSize / (1024 * 1024)) + " MB, sdk=" +
                   (c.second.sdk ? "1" : "0") + " libarchive=" + (c.second.la ? "1" : "0") + ")";
    }
    util::logLine("sevenzip: coder chains: " + summary);

    if (sdkAll) return DecodePlan::Sdk;

    if (laAll) {
        if (maxFolderSize > kSdkMaxFolderSize) {
            util::logLine("sevenzip: archive has large unpack folder (" + std::to_string(maxFolderSize / (1024 * 1024)) +
                          " MB > " + std::to_string(kSdkMaxFolderSize / (1024 * 1024)) +
                          " MB), deferring to streaming libarchive");
        } else {
            util::logLine("sevenzip: archive uses a method the SDK lacks, deferring to libarchive");
        }
        outError = k7zRequiresStreaming;
        return DecodePlan::Libarchive;
    }

    // Neither decoder can do the whole archive: report the chains libarchive
    // rejects (the SDK can't take over once any folder is too big for RAM).
    std::string bad;
    for (const auto& c : chains) {
        if (c.second.la) continue;
        if (!bad.empty()) bad += ", ";
        bad += c.first;
    }
    if (encrypted) {
        outError = "Архив 7z зашифрован паролем, такие архивы не поддерживаются";
    } else if (maxFolderSize > kSdkMaxFolderSize) {
        outError = "Неподдерживаемый метод сжатия 7z: " + bad + " (архив слишком большой для встроенного декодера, " +
                   std::to_string(maxFolderSize / (1024 * 1024)) + " МБ в одном блоке)";
    } else {
        outError = "Неподдерживаемый метод сжатия 7z: " + bad;
    }
    util::logLine("sevenzip: unsupported archive, aborting before reading data: " + outError);
    return DecodePlan::Unsupported;
}

} // namespace

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
    case SZ_ERROR_MEM:          return "Не хватает оперативной памяти (RAM)";
    case SZ_ERROR_NO_ARCHIVE:   return "Файл не является архивом 7z";
    case SZ_ERROR_ARCHIVE:      return "Архив повреждён или имеет неверный формат";
    case SZ_ERROR_CRC:          return "Ошибка контрольной суммы: архив повреждён";
    case SZ_ERROR_UNSUPPORTED:  return "Архив использует неподдерживаемые возможности (например, шифрование)";
    case SZ_ERROR_INPUT_EOF:    return "Архив повреждён (неожиданный конец файла)";
    case SZ_ERROR_FAIL:         return "Не удалось прочитать архив";
    case SZ_ERROR_DATA:         return "Ошибка данных в архиве";
    default:                    return "Неизвестная ошибка декодирования 7z";
    }
}

struct DataSourceInStream {
    ISeekInStream vt;
    datasource::IDataSource* source = nullptr;
    uint64_t totalSize = 0;
    uint64_t currentOffset = 0;
    std::shared_ptr<std::atomic<bool>> cancelToken;
    std::shared_ptr<std::atomic<bool>> pauseToken;
    WRes wres = 0;
};

static SRes DataSourceInStream_Read(ISeekInStreamPtr pp, void *buf, size_t *size) {
    auto* p = const_cast<DataSourceInStream*>(reinterpret_cast<const DataSourceInStream*>(pp));
    if (!p || !p->source || !buf || !size) return SZ_ERROR_FAIL;
    if (p->cancelToken && p->cancelToken->load()) return SZ_ERROR_FAIL;

    if (p->totalSize > 0 && p->currentOffset >= p->totalSize) {
        *size = 0;
        return SZ_OK;
    }

    size_t want = *size;
    if (p->totalSize > 0 && p->currentOffset + want > p->totalSize) {
        want = static_cast<size_t>(p->totalSize - p->currentOffset);
    }
    if (want == 0) {
        *size = 0;
        return SZ_OK;
    }

    size_t got = 0;
    while (!got && !g_appExiting.load()) {
        if (p->cancelToken && p->cancelToken->load()) return SZ_ERROR_FAIL;
        if (p->pauseToken && p->pauseToken->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        got = p->source->read(p->currentOffset, buf, want);
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
    }

    if (got > 0) {
        p->currentOffset += got;
        *size = got;
        return SZ_OK;
    }

    return SZ_ERROR_READ;
}

static SRes DataSourceInStream_Seek(ISeekInStreamPtr pp, Int64 *pos, ESzSeek origin) {
    auto* p = const_cast<DataSourceInStream*>(reinterpret_cast<const DataSourceInStream*>(pp));
    if (!p || !pos) return SZ_ERROR_FAIL;
    if (p->cancelToken && p->cancelToken->load()) return SZ_ERROR_FAIL;

    int64_t newOffset = 0;
    if (origin == SZ_SEEK_SET) {
        newOffset = *pos;
    } else if (origin == SZ_SEEK_CUR) {
        newOffset = static_cast<int64_t>(p->currentOffset) + *pos;
    } else if (origin == SZ_SEEK_END) {
        newOffset = static_cast<int64_t>(p->totalSize) + *pos;
    } else {
        return SZ_ERROR_FAIL;
    }

    if (newOffset < 0 || (p->totalSize > 0 && static_cast<uint64_t>(newOffset) > p->totalSize)) {
        return SZ_ERROR_FAIL;
    }

    p->currentOffset = static_cast<uint64_t>(newOffset);
    *pos = static_cast<Int64>(p->currentOffset);
    return SZ_OK;
}

static void DataSourceInStream_CreateVTable(DataSourceInStream *p) {
    p->vt.Read = DataSourceInStream_Read;
    p->vt.Seek = DataSourceInStream_Seek;
}

static bool extract7zInternal(
    ISeekInStreamPtr inStream,
    uint64_t totalFileSize,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::shared_ptr<std::atomic<bool>> pauseToken,
    std::string& outError,
    uint64_t* outKnownUncompressedSize,
    size_t* outKnownTotalEntries,
    PathTransformFn pathTransform,
    ExtractionJournal* journal,
    bool* outNoFallback
) {
    CLookToRead2 lookStream;
    CSzArEx db;
    SRes res;

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
        lookStream.realStream = inStream;
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
        UInt64 maxFolderSize = 0;
        for (UInt32 fi = 0; fi < db.db.NumFolders; fi++) {
            UInt64 us = SzAr_GetFolderUnpackSize(&db.db, fi);
            if (us > maxFolderSize) maxFolderSize = us;
        }
        for (UInt32 fi = 0; fi < db.NumFiles; fi++) {
            if (!SzArEx_IsDir(&db, fi)) {
                totalUncompressed += SzArEx_GetFileSize(&db, fi);
            }
        }
        progress.totalUncompressedSize = totalUncompressed;
        progress.totalEntries = db.NumFiles;
        if (outKnownUncompressedSize) *outKnownUncompressedSize = totalUncompressed;
        if (outKnownTotalEntries) *outKnownTotalEntries = db.NumFiles;

        const DecodePlan plan = plan7zDecode(db, maxFolderSize, outError);
        if (plan != DecodePlan::Sdk) {
            if (plan == DecodePlan::Unsupported && outNoFallback) *outNoFallback = true;
            SzFree(nullptr, lookStream.buf);
            SzArEx_Free(&db, (ISzAllocPtr)&g_allocImp);
            return false;
        }

        auto makeDirs = [journal](const std::string& dir) {
            if (journal) journal->createDirectories(dir);
            else safeCreateDirectories(dir);
        };

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
            if (pauseToken && pauseToken->load()) {
                while (pauseToken->load() && (!cancelToken || !cancelToken->load()) && !g_appExiting.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (cancelToken && cancelToken->load()) {
                    outError = "Распаковка отменена пользователем";
                    break;
                }
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

            std::string targetPath = cleanName;
            if (pathTransform) {
                targetPath = pathTransform(cleanName, isDir);
                if (targetPath.empty()) {
                    continue;
                }
            }

            progress.currentFileName = cleanName;
            progress.entriesProcessed++;

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

            if (progress.entriesProcessed == 1 || (progress.entriesProcessed % 50 == 0)) {
                util::logLine("sevenzip: extracting entry #" + std::to_string(progress.entriesProcessed) + ": " + cleanName);
            }

            if (isDir) {
                makeDirs(fullPath);
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
                if (res == SZ_ERROR_MEM) {
                    outError = k7zRequiresStreaming;
                } else {
                    outError = sresToMessage(res);
                }
                util::logLine("sevenzip: SzArEx_Extract failed for " + cleanName +
                              ": " + std::to_string((int)res) + " (" + outError + ")");
                break;
            }

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
                makeDirs(fullPath.substr(0, lastSlash));
            }
            if (journal) journal->noteFile(fullPath);

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
            outError = res == SZ_OK ? "Распаковка прервана" : sresToMessage(res);
        }
    }

    SzFree(nullptr, outBuffer);
    SzFree(nullptr, nameBuf);
    SzFree(nullptr, lookStream.buf);
    lookStream.buf = nullptr;
    SzArEx_Free(&db, (ISzAllocPtr)&g_allocImp);
    return ok;
}

bool extract7zArchive(
    const std::string& archivePath,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError,
    uint64_t* outKnownUncompressedSize,
    size_t* outKnownTotalEntries,
    PathTransformFn pathTransform,
    ExtractionJournal* journal,
    bool* outNoFallback
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
        if (journal) journal->createDirectories(destinationDir);
        else safeCreateDirectories(destinationDir);
    } catch (const std::exception& e) {
        outError = e.what();
        util::logLine("sevenzip: exception preparing destination: " + std::string(e.what()));
        return false;
    }

    CFileInStream archiveStream;
    File_Construct(&archiveStream.file);
    archiveStream.wres = 0;

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

    FileInStream_CreateVTable(&archiveStream);

    bool ok = extract7zInternal(&archiveStream.vt, totalFileSize, destinationDir,
                                progressCb, cancelToken, nullptr, outError,
                                outKnownUncompressedSize, outKnownTotalEntries, pathTransform,
                                journal, outNoFallback);

    File_Close(&archiveStream.file);
    util::logLine("sevenzip: extractArchive finished, success=" + std::string(ok ? "1" : "0"));
    return ok;
}

bool extract7zArchiveFromSource(
    datasource::IDataSource* source,
    uint64_t archiveSize,
    const std::string& destinationDir,
    std::function<void(const ArchiveProgress&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::shared_ptr<std::atomic<bool>> pauseToken,
    std::string& outError,
    uint64_t* outKnownUncompressedSize,
    size_t* outKnownTotalEntries,
    PathTransformFn pathTransform,
    ExtractionJournal* journal,
    bool* outNoFallback
) {
    if (!source) {
        outError = "Null data source";
        return false;
    }
    util::logLine("sevenzip: extract7zArchiveFromSource start, size=" + std::to_string(archiveSize) + " -> " + destinationDir);
    if (journal) journal->createDirectories(destinationDir);
    else safeCreateDirectories(destinationDir);

    DataSourceInStream dsStream;
    dsStream.source = source;
    dsStream.totalSize = archiveSize;
    dsStream.currentOffset = 0;
    dsStream.cancelToken = cancelToken;
    dsStream.pauseToken = pauseToken;
    dsStream.wres = 0;
    DataSourceInStream_CreateVTable(&dsStream);

    bool ok = extract7zInternal(&dsStream.vt, archiveSize, destinationDir,
                                progressCb, cancelToken, pauseToken, outError,
                                outKnownUncompressedSize, outKnownTotalEntries, pathTransform,
                                journal, outNoFallback);
    util::logLine("sevenzip: extract7zArchiveFromSource finished, success=" + std::string(ok ? "1" : "0"));
    return ok;
}

bool list7zArchiveFolder(
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

    CFileInStream archiveStream;
    CLookToRead2 lookStream;
    CSzArEx db;
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
            outError = "Не удалось открыть 7z архив";
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

    lookStream.buf = static_cast<Byte*>(SzAlloc(nullptr, kInputBufSize));
    if (!lookStream.buf) {
        File_Close(&archiveStream.file);
        outError = "Не хватает оперативной памяти (RAM)";
        return false;
    }
    lookStream.bufSize = kInputBufSize;
    lookStream.realStream = &archiveStream.vt;
    LookToRead2_INIT(&lookStream);

    SRes res = SzArEx_Open(&db, &lookStream.vt, (ISzAllocPtr)&g_allocImp, (ISzAllocPtr)&g_allocTempImp);
    if (res != SZ_OK) {
        outError = sresToMessage(res);
        SzFree(nullptr, lookStream.buf);
        File_Close(&archiveStream.file);
        return false;
    }

    UInt16* nameBuf = nullptr;
    size_t nameBufSize = 0;
    std::unordered_set<std::string> seenDirs;
    std::unordered_set<std::string> seenFiles;

    for (UInt32 i = 0; i < db.NumFiles; i++) {
        size_t nameLen = SzArEx_GetFileNameUtf16(&db, i, nullptr);
        if (nameLen == 0 || nameLen > (1u << 20)) continue;
        if (nameLen > nameBufSize) {
            SzFree(nullptr, nameBuf);
            nameBufSize = nameLen;
            nameBuf = static_cast<UInt16*>(SzAlloc(nullptr, nameBufSize * sizeof(UInt16)));
            if (!nameBuf) break;
        }
        SzArEx_GetFileNameUtf16(&db, i, nameBuf);
        std::string rawName = utf16leToUtf8(nameBuf, nameLen - 1);
        std::string clean = sanitizeEntryName(rawName);
        for (char& c : clean) {
            if (c == '\\') c = '/';
        }
        if (clean.empty()) continue;

        bool isDir = (SzArEx_IsDir(&db, i) != 0) || (!clean.empty() && clean.back() == '/');
        while (!clean.empty() && clean.back() == '/') clean.pop_back();
        if (clean.empty()) continue;

        uint64_t fileSize = isDir ? 0 : SzArEx_GetFileSize(&db, i);

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
                        outItems.push_back(item);
                    }
                } else {
                    if (seenFiles.insert(clean).second) {
                        FileItem item;
                        item.name = clean;
                        item.path = archivePath + "/" + clean;
                        item.isDir = false;
                        item.size = fileSize;
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
                    outItems.push_back(dirItem);
                }
            }
        } else {
            std::string prefix = normInner + "/";
            if (clean.compare(0, prefix.size(), prefix) == 0) {
                std::string rel = clean.substr(prefix.size());
                if (rel.empty()) continue;
                size_t slash = rel.find('/');
                if (slash == std::string::npos) {
                    if (isDir) {
                        if (seenDirs.insert(rel).second) {
                            FileItem item;
                            item.name = rel;
                            item.path = archivePath + "/" + clean;
                            item.isDir = true;
                            item.size = 0;
                            outItems.push_back(item);
                        }
                    } else {
                        if (seenFiles.insert(rel).second) {
                            FileItem item;
                            item.name = rel;
                            item.path = archivePath + "/" + clean;
                            item.isDir = false;
                            item.size = fileSize;
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
                        outItems.push_back(dirItem);
                    }
                }
            }
        }
    }

    SzFree(nullptr, nameBuf);
    SzFree(nullptr, lookStream.buf);
    lookStream.buf = nullptr;
    SzArEx_Free(&db, (ISzAllocPtr)&g_allocImp);
    File_Close(&archiveStream.file);

    std::sort(outItems.begin(), outItems.end(), [](const FileItem& a, const FileItem& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return a.name < b.name;
    });

    return true;
}

bool get7zArchiveTotals(
    const std::string& archivePath,
    uint64_t& outUncompressedSize,
    size_t& outTotalEntries
) {
    outUncompressedSize = 0;
    outTotalEntries = 0;

    CFileInStream archiveStream;
    CLookToRead2 lookStream;
    CSzArEx db;
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

    lookStream.buf = static_cast<Byte*>(SzAlloc(nullptr, kInputBufSize));
    if (!lookStream.buf) {
        File_Close(&archiveStream.file);
        return false;
    }
    lookStream.bufSize = kInputBufSize;
    lookStream.realStream = &archiveStream.vt;
    LookToRead2_INIT(&lookStream);

    SRes res = SzArEx_Open(&db, &lookStream.vt, (ISzAllocPtr)&g_allocImp, (ISzAllocPtr)&g_allocTempImp);
    if (res != SZ_OK) {
        SzFree(nullptr, lookStream.buf);
        File_Close(&archiveStream.file);
        return false;
    }

    UInt64 totalUncompressed = 0;
    for (UInt32 fi = 0; fi < db.NumFiles; fi++) {
        if (!SzArEx_IsDir(&db, fi)) {
            totalUncompressed += SzArEx_GetFileSize(&db, fi);
        }
    }
    outUncompressedSize = totalUncompressed;
    outTotalEntries = db.NumFiles;

    SzFree(nullptr, lookStream.buf);
    lookStream.buf = nullptr;
    SzArEx_Free(&db, (ISzAllocPtr)&g_allocImp);
    File_Close(&archiveStream.file);
    return true;
}

bool iterate7zArchiveEntries(
    const std::string& archivePath,
    std::function<bool(const std::string& entryName, bool isDir)> callback
) {
    if (!callback) return false;

    CFileInStream archiveStream;
    CLookToRead2 lookStream;
    CSzArEx db;
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
        if (wres != 0) return false;
    }

    FileInStream_CreateVTable(&archiveStream);
    archiveStream.wres = 0;

    LookToRead2_CreateVTable(&lookStream, False);
    lookStream.buf = nullptr;
    lookStream.bufSize = 0;
    lookStream.realStream = nullptr;

    CrcGenerateTable();
    SzArEx_Init(&db);

    lookStream.buf = static_cast<Byte*>(SzAlloc(nullptr, kInputBufSize));
    if (!lookStream.buf) {
        File_Close(&archiveStream.file);
        return false;
    }
    lookStream.bufSize = kInputBufSize;
    lookStream.realStream = &archiveStream.vt;
    LookToRead2_INIT(&lookStream);

    SRes res = SzArEx_Open(&db, &lookStream.vt, (ISzAllocPtr)&g_allocImp, (ISzAllocPtr)&g_allocTempImp);
    if (res != SZ_OK) {
        SzFree(nullptr, lookStream.buf);
        File_Close(&archiveStream.file);
        return false;
    }

    UInt16* nameBuf = nullptr;
    size_t nameBufSize = 0;
    bool keepGoing = true;

    for (UInt32 i = 0; i < db.NumFiles && keepGoing; i++) {
        size_t nameLen = SzArEx_GetFileNameUtf16(&db, i, nullptr);
        if (nameLen == 0 || nameLen > (1u << 20)) continue;
        if (nameLen > nameBufSize) {
            SzFree(nullptr, nameBuf);
            nameBufSize = nameLen;
            nameBuf = static_cast<UInt16*>(SzAlloc(nullptr, nameBufSize * sizeof(UInt16)));
            if (!nameBuf) break;
        }
        SzArEx_GetFileNameUtf16(&db, i, nameBuf);
        std::string rawName = utf16leToUtf8(nameBuf, nameLen - 1);
        std::string clean = sanitizeEntryName(rawName);
        for (char& c : clean) {
            if (c == '\\') c = '/';
        }
        if (clean.empty()) continue;

        bool isDir = (SzArEx_IsDir(&db, i) != 0) || (!clean.empty() && clean.back() == '/');
        while (!clean.empty() && clean.back() == '/') clean.pop_back();
        if (clean.empty()) continue;

        keepGoing = callback(clean, isDir);
    }

    if (nameBuf) SzFree(nullptr, nameBuf);
    SzFree(nullptr, lookStream.buf);
    SzArEx_Free(&db, (ISzAllocPtr)&g_allocImp);
    File_Close(&archiveStream.file);
    return true;
}

} // namespace util
