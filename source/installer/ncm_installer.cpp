#include "ncm_installer.h"
#include "nexus_usb_client.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <limits>
#include "../utils/log.h"

#ifdef __SWITCH__
#include <mutex>
#include "../engine/engine_log.h"
extern std::recursive_mutex g_switch_service_mutex;

// [perf] inst: time inside ncmContentStorageWritePlaceHolder, one line per 5 s
// to engine.log (one writing thread at a time). The SD write path (ncm -> fs) runs on
// the system core next to bsd and vi; this lets the netloop's slow-socket
// windows (sock_us/call) be lined up with slow writes.
namespace {
u64 g_pf_tick = 0, g_pf_sum = 0, g_pf_max = 0;
uint32_t g_pf_n = 0;
uint64_t g_pf_bytes = 0;

void perfWrite(u64 dt, size_t bytes) {
    u64 now = armGetSystemTick();
    if (g_pf_tick == 0) g_pf_tick = now;
    g_pf_sum += dt;
    if (dt > g_pf_max) g_pf_max = dt;
    g_pf_n++;
    g_pf_bytes += bytes;
    const u64 freq = armGetSystemTickFreq();
    if (now - g_pf_tick >= 5 * freq) {
        double secs = (double)(now - g_pf_tick) / (double)freq;
        engine_log(ENGINE_LOG_INFO,
                   "[perf] inst writes=%u rate=%.0fKB/s write_ms/s=%.0f max_ms=%.1f avg_ms=%.2f",
                   g_pf_n, (double)g_pf_bytes / 1024.0 / secs,
                   (double)g_pf_sum * 1000.0 / (double)freq / secs,
                   (double)g_pf_max * 1000.0 / (double)freq,
                   g_pf_n ? (double)g_pf_sum * 1000.0 / (double)freq / g_pf_n : 0.0);
        g_pf_tick = now;
        g_pf_sum = g_pf_max = 0;
        g_pf_n = 0;
        g_pf_bytes = 0;
    }
}

std::string formatHex32(Result rc) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(rc));
    return std::string(buf);
}

void inspectUsbStatusFile(bool& out_route_disabled, bool& out_read_only) {
    out_route_disabled = false;
    out_read_only = false;

    FILE* f = std::fopen("sdmc:/atmosphere/logs/usb_storage_status.txt", "rb");
    if (!f) return;

    char buf[4096];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';

    const char* p = buf;
    while (*p != '\0') {
        const char* eol = std::strchr(p, '\n');
        size_t len = eol ? static_cast<size_t>(eol - p) : std::strlen(p);
        if (len > 0 && p[len - 1] == '\r') {
            --len;
        }
        if (len == 15 && std::memcmp(p, "install_route=0", 15) == 0) {
            out_route_disabled = true;
        } else if (len == 10 && std::memcmp(p, "writable=0", 10) == 0) {
            out_read_only = true;
        }
        if (!eol) break;
        p = eol + 1;
    }
}

bool endsWithInsensitive(const std::string& value, const std::string& suffix) {
    if (value.size() < suffix.size()) return false;
    auto from = value.end() - static_cast<std::ptrdiff_t>(suffix.size());
    for (size_t i = 0; i < suffix.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(from[i]);
        unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

bool readCnmtFileFromFsPath(const char* content_path, std::vector<uint8_t>& out_buf) {
    if (!content_path || content_path[0] == '\0') return false;

    FsFileSystem cnmt_fs = {};
    Result rc = fsOpenFileSystemWithId(&cnmt_fs,
                                       0,
                                       FsFileSystemType_ContentMeta,
                                       content_path,
                                       FsContentAttributes_None);
    if (R_FAILED(rc)) {
        rc = fsOpenFileSystemWithId(&cnmt_fs,
                                    0,
                                    FsFileSystemType_ContentMeta,
                                    content_path,
                                    FsContentAttributes_All);
    }
    if (R_FAILED(rc)) {
        util::logLine("ncm: fsOpenFileSystemWithId(ContentMeta) failed, rc=" + formatHex32(rc) +
                      " path=" + std::string(content_path));
        return false;
    }

    FsDir dir = {};
    rc = fsFsOpenDirectory(&cnmt_fs, "/", FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) {
        util::logLine("ncm: fsFsOpenDirectory failed for content meta fs, rc=" + formatHex32(rc));
        fsFsClose(&cnmt_fs);
        return false;
    }

    std::string cnmt_name;
    while (cnmt_name.empty()) {
        FsDirectoryEntry entries[8] = {};
        s64 read_count = 0;
        rc = fsDirRead(&dir, &read_count, 8, entries);
        if (R_FAILED(rc)) {
            util::logLine("ncm: fsDirRead failed for content meta fs, rc=" + formatHex32(rc));
            break;
        }
        if (read_count <= 0) break;

        for (s64 i = 0; i < read_count; ++i) {
            if (entries[i].type != FsDirEntryType_File) continue;
            std::string name(entries[i].name);
            if (endsWithInsensitive(name, ".cnmt")) {
                cnmt_name = name;
                break;
            }
        }
    }

    fsDirClose(&dir);

    if (cnmt_name.empty()) {
        util::logLine("ncm: no .cnmt file found in ContentMeta fs");
        fsFsClose(&cnmt_fs);
        return false;
    }

    std::string cnmt_path = "/" + cnmt_name;
    FsFile cnmt_file = {};
    rc = fsFsOpenFile(&cnmt_fs, cnmt_path.c_str(), FsOpenMode_Read, &cnmt_file);
    if (R_FAILED(rc)) {
        util::logLine("ncm: fsFsOpenFile failed for " + cnmt_path + ", rc=" + formatHex32(rc));
        fsFsClose(&cnmt_fs);
        return false;
    }

    s64 cnmt_size = 0;
    rc = fsFileGetSize(&cnmt_file, &cnmt_size);
    if (R_FAILED(rc) || cnmt_size <= 0) {
        util::logLine("ncm: fsFileGetSize failed for cnmt, rc=" + formatHex32(rc));
        fsFileClose(&cnmt_file);
        fsFsClose(&cnmt_fs);
        return false;
    }

    if (static_cast<u64>(cnmt_size) > static_cast<u64>(std::numeric_limits<size_t>::max())) {
        util::logLine("ncm: cnmt file is too large to buffer: " + std::to_string(cnmt_size));
        fsFileClose(&cnmt_file);
        fsFsClose(&cnmt_fs);
        return false;
    }

    out_buf.resize(static_cast<size_t>(cnmt_size));
    u64 read_size = 0;
    rc = fsFileRead(&cnmt_file,
                    0,
                    out_buf.data(),
                    out_buf.size(),
                    FsReadOption_None,
                    &read_size);
    fsFileClose(&cnmt_file);
    fsFsClose(&cnmt_fs);

    if (R_FAILED(rc) || read_size != out_buf.size()) {
        util::logLine("ncm: fsFileRead failed for cnmt, rc=" + formatHex32(rc));
        out_buf.clear();
        return false;
    }

    return true;
}
} // namespace
#endif

namespace installer {

NcmInstaller::NcmInstaller() = default;

NcmInstaller::~NcmInstaller() {
#ifdef __SWITCH__
    if (initialized_) {
        cleanup();
    }
    if (db_opened_) {
        ncmContentMetaDatabaseClose(&meta_db_);
        db_opened_ = false;
    }
    if (storage_opened_) {
        ncmContentStorageClose(&content_storage_);
        storage_opened_ = false;
    }
    if (initialized_) {
        ncmExit();
        initialized_ = false;
    }
#endif
}

#ifdef __SWITCH__

bool NcmInstaller::begin(StorageTarget target, uint64_t usb_generation) {
    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
    if (initialized_) {
        util::logLine("ncm: already initialized, resetting...");
        cleanup();
        if (db_opened_) {
            ncmContentMetaDatabaseClose(&meta_db_);
            db_opened_ = false;
        }
        if (storage_opened_) {
            ncmContentStorageClose(&content_storage_);
            storage_opened_ = false;
        }
        ncmExit();
        initialized_ = false;
    }

    target_ = target;
    usb_generation_ = usb_generation;
    last_error_.clear();

    if (target_ == StorageTarget::Usb) {
        storage_id_ = NcmStorageId_SdCard;
    } else {
        storage_id_ = (target_ == StorageTarget::Nand) ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
    }

    // Инициализация сервиса ncm
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        last_error_ = "ncmInitialize failed (" + formatHex32(rc) + ")";
        util::logLine("ncm: " + last_error_);
        return false;
    }

    if (target_ == StorageTarget::Usb) {
        if (!NexusUsbClient::instance().isSupported()) {
            last_error_ = "Nexus USB not supported by firmware";
            util::logLine("ncm: " + last_error_);
            ncmExit();
            return false;
        }

        if (usb_generation_ == 0) {
            NexusUsbStatus status = {};
            if (NexusUsbClient::instance().queryStatus(status) && status.isAvailable()) {
                usb_generation_ = status.generation;
            } else {
                last_error_ = "USB drive not ready";
                util::logLine("ncm: " + last_error_);
                ncmExit();
                return false;
            }
        }

        rc = NexusUsbClient::instance().openPinnedContentStorage(usb_generation_, &content_storage_);
        if (R_FAILED(rc)) {
            if (rc == MAKERESULT(505, 900)) {
                last_error_ = "USB writes blocked: pending repair/import journal on SD (" + formatHex32(rc) + ")";
            } else if (rc == MAKERESULT(140, 208) || rc == MAKERESULT(2, 2001)) {
                bool route_disabled = false;
                bool read_only = false;
                inspectUsbStatusFile(route_disabled, read_only);
                NexusUsbStatus cur_status = {};
                const bool status_ok = NexusUsbClient::instance().queryStatus(cur_status);
                if (route_disabled) {
                    last_error_ = "USB install route disabled in Nexus overlay (" + formatHex32(rc) + ")";
                } else if (read_only) {
                    last_error_ = "USB drive is mounted read-only (" + formatHex32(rc) + ")";
                } else if (!status_ok || !cur_status.isAvailable() || cur_status.generation != usb_generation_) {
                    last_error_ = "USB drive disconnected or changed (" + formatHex32(rc) + ")";
                } else {
                    last_error_ = "USB openPinnedContentStorage failed (" + formatHex32(rc) + ")";
                }
            } else {
                last_error_ = "USB openPinnedContentStorage failed (" + formatHex32(rc) + ")";
            }
            util::logLine("ncm: " + last_error_);
            ncmExit();
            return false;
        }
        storage_opened_ = true;

        // ContentMetaDatabase для USB-игр размещается на SD-карте
        rc = ncmOpenContentMetaDatabase(&meta_db_, NcmStorageId_SdCard);
        if (R_FAILED(rc)) {
            last_error_ = "ncmOpenContentMetaDatabase(SdCard) failed (" + formatHex32(rc) + ")";
            util::logLine("ncm: " + last_error_);
            ncmContentStorageClose(&content_storage_);
            storage_opened_ = false;
            ncmExit();
            return false;
        }
        db_opened_ = true;
    } else {
        // Открыть ContentStorage для выбранного хранилища
        if (target_ == StorageTarget::SdCard && NexusUsbClient::instance().isSupported()) {
            rc = NexusUsbClient::instance().openNativeSdContentStorage(&content_storage_);
            if (R_FAILED(rc)) {
                util::logLine("ncm: openNativeSdContentStorage failed (" + formatHex32(rc) +
                              "), falling back to ncmOpenContentStorage");
                rc = ncmOpenContentStorage(&content_storage_, storage_id_);
            }
        } else {
            rc = ncmOpenContentStorage(&content_storage_, storage_id_);
        }
        if (R_FAILED(rc)) {
            last_error_ = "ncmOpenContentStorage failed (" + formatHex32(rc) + ")";
            util::logLine("ncm: " + last_error_);
            ncmExit();
            return false;
        }
        storage_opened_ = true;

        // Открыть ContentMetaDatabase
        rc = ncmOpenContentMetaDatabase(&meta_db_, storage_id_);
        if (R_FAILED(rc)) {
            last_error_ = "ncmOpenContentMetaDatabase failed (" + formatHex32(rc) + ")";
            util::logLine("ncm: " + last_error_);
            ncmContentStorageClose(&content_storage_);
            storage_opened_ = false;
            ncmExit();
            return false;
        }
        db_opened_ = true;
    }

    initialized_ = true;
    util::logLine("ncm: initialized, target=" + std::to_string(static_cast<int>(target_)) +
                  " storage=" + std::to_string(static_cast<int>(storage_id_)) +
                  " usb_gen=" + std::to_string(usb_generation_));
    return true;
}

bool NcmInstaller::begin(NcmStorageId storage) {
    StorageTarget target = (storage == NcmStorageId_BuiltInUser) ? StorageTarget::Nand : StorageTarget::SdCard;
    return begin(target, 0);
}

bool NcmInstaller::createPlaceHolder(const NcmContentId& id, uint64_t size) {
    if (!initialized_) return false;

    // Р“РµРЅРµСЂРёСЂСѓРµРј PlaceHolderId РЅР° РѕСЃРЅРѕРІРµ ContentId
    NcmPlaceHolderId placeholder_id;
    std::memcpy(&placeholder_id, &id, sizeof(NcmPlaceHolderId));

    // РЈРґР°Р»СЏРµРј СЃС‚Р°СЂС‹Р№ РїР»РµР№СЃС…РѕР»РґРµСЂ, РµСЃР»Рё РµСЃС‚СЊ (РёРіРЅРѕСЂРёСЂСѓРµРј РѕС€РёР±РєСѓ)
    ncmContentStorageDeletePlaceHolder(&content_storage_, &placeholder_id);

    Result rc = ncmContentStorageCreatePlaceHolder(&content_storage_, &id,
                                                    &placeholder_id,
                                                    static_cast<s64>(size));
    if (R_FAILED(rc)) {
        if (R_MODULE(rc) == 2 && R_DESCRIPTION(rc) == 39) {
            util::logLine("ncm: createPlaceHolder failed: Not enough free space on storage (requires " +
                          std::to_string((size + 1024*1024*1024 - 1) / (1024*1024*1024)) + " GB, rc=" + formatHex32(rc) + ")");
        } else {
            util::logLine("ncm: createPlaceHolder failed, rc=" + formatHex32(rc));
        }
        return false;
    }

    placeholders_.push_back(id);
    return true;
}

bool NcmInstaller::writePlaceHolder(const NcmContentId& id,
                                     uint64_t offset,
                                     const void* data, size_t size) {
    if (!initialized_ || !data || size == 0) return false;

    NcmPlaceHolderId placeholder_id;
    std::memcpy(&placeholder_id, &id, sizeof(NcmPlaceHolderId));

    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    uint64_t current_offset = offset;
    constexpr size_t kMaxChunkSize = 512 * 1024; // 512 KB

    while (remaining > 0) {
        size_t chunk = std::min(remaining, kMaxChunkSize);
        u64 w0 = armGetSystemTick();
        Result rc = ncmContentStorageWritePlaceHolder(&content_storage_,
                                                       &placeholder_id,
                                                       static_cast<s64>(current_offset),
                                                       ptr,
                                                       chunk);
        perfWrite(armGetSystemTick() - w0, chunk);
        if (R_FAILED(rc)) {
            util::logLine("ncm: writePlaceHolder failed, offset="
                           + std::to_string(current_offset)
                           + " size=" + std::to_string(chunk)
                           + " rc=" + formatHex32(rc));
            return false;
        }
        ptr += chunk;
        current_offset += chunk;
        remaining -= chunk;
    }

    return true;
}

bool NcmInstaller::finalizePlaceHolder(const NcmContentId& id) {
    if (!initialized_) return false;

    NcmPlaceHolderId placeholder_id;
    std::memcpy(&placeholder_id, &id, sizeof(NcmPlaceHolderId));

    // Content IDs are content hashes: an existing ID is the same NCA the current install still uses.
    bool existed = false;
    if (R_FAILED(ncmContentStorageHas(&content_storage_, &existed, &id))) existed = false;

    // РЈРґР°Р»СЏРµРј СЃСѓС‰РµСЃС‚РІСѓСЋС‰РёР№ РєРѕРЅС‚РµРЅС‚, РµСЃР»Рё РµСЃС‚СЊ (РїРµСЂРµСѓСЃС‚Р°РЅРѕРІРєР°)
    ncmContentStorageDelete(&content_storage_, &id);

    // Р РµРіРёСЃС‚СЂРёСЂСѓРµРј РїР»РµР№СЃС…РѕР»РґРµСЂ РєР°Рє РіРѕС‚РѕРІС‹Р№ РєРѕРЅС‚РµРЅС‚
    Result rc = ncmContentStorageRegister(&content_storage_, &id, &placeholder_id);
    if (R_FAILED(rc)) {
        util::logLine("ncm: register failed, rc=" + formatHex32(rc));
        return false;
    }
    if (existed) {
        util::logLine("ncm: content already installed, kept out of rollback");
    } else {
        registered_.push_back(id);
    }

    // Убираем из списка плейсхолдеров
    for (auto it = placeholders_.begin(); it != placeholders_.end(); ++it) {
        if (std::memcmp(&(*it), &id, sizeof(NcmContentId)) == 0) {
            placeholders_.erase(it);
            break;
        }
    }

    if (target_ == StorageTarget::Usb) {
        s64 content_size = 0;
        Result size_rc = ncmContentStorageGetSizeFromContentId(&content_storage_, &content_size, &id);
        if (R_FAILED(size_rc) || content_size <= 0) {
            util::logLine("ncm: USB get content size failed after register, rc=" + formatHex32(size_rc) +
                          " size=" + std::to_string(content_size));
            return false;
        }
        Result reg_rc = NexusUsbClient::instance().ensureContentRegistered(usb_generation_, id, content_size);
        if (R_FAILED(reg_rc)) {
            util::logLine("ncm: USB ensureContentRegistered failed, rc=" + formatHex32(reg_rc));
            return false;
        }
    }

    return true;
}

bool NcmInstaller::deletePlaceHolder(const NcmContentId& id) {
    if (!initialized_) return false;

    NcmPlaceHolderId placeholder_id;
    std::memcpy(&placeholder_id, &id, sizeof(NcmPlaceHolderId));

    Result rc = ncmContentStorageDeletePlaceHolder(&content_storage_, &placeholder_id);
    if (R_FAILED(rc)) {
        return false;
    }

    for (auto it = placeholders_.begin(); it != placeholders_.end(); ++it) {
        if (std::memcmp(&(*it), &id, sizeof(NcmContentId)) == 0) {
            placeholders_.erase(it);
            break;
        }
    }

    return true;
}

bool NcmInstaller::registerContentMeta(const NcmContentMetaKey& key,
                                        const void* cnmt_data, size_t cnmt_size) {
    if (!initialized_ || !cnmt_data || cnmt_size == 0) return false;

    // Р—Р°РїРёСЃС‹РІР°РµРј CNMT РІ ContentMetaDatabase
    Result rc = ncmContentMetaDatabaseSet(&meta_db_, &key, cnmt_data, cnmt_size);
    if (R_FAILED(rc)) {
        util::logLine("ncm: contentMetaDatabaseSet failed, rc=" + std::to_string(rc));
        return false;
    }

    // РљРѕРјРјРёС‚РёРј РёР·РјРµРЅРµРЅРёСЏ
    rc = ncmContentMetaDatabaseCommit(&meta_db_);
    if (R_FAILED(rc)) {
        util::logLine("ncm: contentMetaDatabaseCommit failed, rc=" + std::to_string(rc));
        return false;
    }

    util::logLine("ncm: metadata registered, type="
                   + std::to_string(static_cast<int>(key.type))
                   + " id=" + std::to_string(key.id));
    return true;
}

bool NcmInstaller::getStoragePath(const NcmContentId& id, char* out_path, size_t path_size) {
    if (!initialized_ || !out_path || path_size == 0) return false;

    // Fix argument order for ncmContentStorageGetPath
    NcmContentId id_copy = id;
    Result rc = ncmContentStorageGetPath(&content_storage_, out_path, path_size, &id_copy);
    if (R_FAILED(rc)) {
        util::logLine("ncm: getStoragePath failed, rc=" + std::to_string(rc));
        return false;
    }

    util::logLine("ncm: storage path = " + std::string(out_path));
    return true;
}

bool NcmInstaller::readContentIdFile(const NcmContentId& id, std::vector<uint8_t>& out_buf) {
    if (!initialized_) return false;

    s64 content_size = 0;
    Result rc = ncmContentStorageGetSizeFromContentId(&content_storage_, &content_size, &id);
    if (R_FAILED(rc)) {
        util::logLine("ncm: get size from content id failed, rc=" + formatHex32(rc));
        return false;
    }
    if (content_size <= 0) {
        util::logLine("ncm: content size is invalid: " + std::to_string(content_size));
        return false;
    }
    if (static_cast<u64>(content_size) > static_cast<u64>(std::numeric_limits<size_t>::max())) {
        util::logLine("ncm: content size is too large to buffer: " + std::to_string(content_size));
        return false;
    }

    out_buf.resize(static_cast<size_t>(content_size));
    constexpr size_t kReadChunkSize = 512 * 1024;
    size_t offset = 0;
    while (offset < out_buf.size()) {
        const size_t chunk = std::min(out_buf.size() - offset, kReadChunkSize);
        rc = ncmContentStorageReadContentIdFile(&content_storage_,
                                                out_buf.data() + offset,
                                                chunk,
                                                &id,
                                                static_cast<s64>(offset));
        if (R_FAILED(rc)) {
            util::logLine("ncm: read content id file failed, offset=" + std::to_string(offset) +
                          " size=" + std::to_string(chunk) +
                          " rc=" + formatHex32(rc));
            out_buf.clear();
            return false;
        }
        offset += chunk;
    }

    return true;
}

bool NcmInstaller::getContentSize(const NcmContentId& id, uint64_t& out_size) {
    if (!initialized_) return false;

    s64 content_size = 0;
    Result rc = ncmContentStorageGetSizeFromContentId(&content_storage_, &content_size, &id);
    if (R_FAILED(rc) || content_size <= 0) {
        return false;
    }

    out_size = static_cast<uint64_t>(content_size);
    return true;
}

bool NcmInstaller::deleteContent(const NcmContentId& id) {
    if (!initialized_) return false;
    return R_SUCCEEDED(ncmContentStorageDelete(&content_storage_, &id));
}

bool NcmInstaller::readCnmtFromContentMetaFs(const NcmContentId& id, std::vector<uint8_t>& out_buf) {
    if (!initialized_) return false;

    if (target_ != StorageTarget::Usb) {
        char content_path[FS_MAX_PATH];
        if (!getStoragePath(id, content_path, sizeof(content_path))) {
            return false;
        }
        return readCnmtFileFromFsPath(content_path, out_buf);
    }

    // On Nexus-Atmosphere, FsMitmService::OpenFileSystemWithId does not intercept
    // FsFileSystemType_ContentMeta for homebrew clients, so stock fsp-srv cannot
    // open @SdCardContent://registered/... files that live only on USB.
    // Stage the encrypted .cnmt.nca into a temporary placeholder on NAND (or native SD),
    // let Horizon's fsp-srv decrypt and read the inner *.cnmt file, then delete it.
    std::vector<uint8_t> nca_buf;
    if (!readContentIdFile(id, nca_buf)) {
        return false;
    }
    if (nca_buf.size() < 0xC00 || nca_buf.size() > 64ULL * 1024 * 1024) {
        util::logLine("ncm: USB CNMT NCA size out of range: " + std::to_string(nca_buf.size()));
        return false;
    }

    struct NativeStageGuard {
        NcmContentStorage cs = {};
        bool cs_open = false;
        NcmPlaceHolderId ph_id = {};
        bool ph_owned = false;

        ~NativeStageGuard() { reset(); }

        void reset() {
            if (!cs_open && !ph_owned) return;
            std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
            if (ph_owned && cs_open) {
                ncmContentStorageDeletePlaceHolder(&cs, &ph_id);
                ph_owned = false;
            }
            if (cs_open) {
                ncmContentStorageClose(&cs);
                cs_open = false;
            }
        }
    };

    auto stageToNative = [&](NativeStageGuard& stage, bool use_native_sd, char* out_path, size_t out_path_size) -> bool {
        stage.reset();

        Result rc = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
            rc = use_native_sd
                ? NexusUsbClient::instance().openNativeSdContentStorage(&stage.cs)
                : ncmOpenContentStorage(&stage.cs, NcmStorageId_BuiltInUser);
            if (R_FAILED(rc)) {
                util::logLine(std::string("ncm: stage open ") + (use_native_sd ? "native SD" : "NAND") +
                              " failed, rc=" + formatHex32(rc));
                return false;
            }
            stage.cs_open = true;
        }

        rc = ncmContentStorageGeneratePlaceHolderId(&stage.cs, &stage.ph_id);
        if (R_FAILED(rc)) {
            util::logLine("ncm: stage GeneratePlaceHolderId failed, rc=" + formatHex32(rc));
            stage.reset();
            return false;
        }

        NcmContentId stage_cid = {};
        std::memcpy(&stage_cid, &stage.ph_id, sizeof(NcmContentId));

        rc = ncmContentStorageCreatePlaceHolder(&stage.cs, &stage_cid, &stage.ph_id, static_cast<s64>(nca_buf.size()));
        if (R_FAILED(rc)) {
            util::logLine("ncm: stage CreatePlaceHolder failed, rc=" + formatHex32(rc));
            stage.reset();
            return false;
        }
        stage.ph_owned = true;

        constexpr size_t kWriteChunkSize = 512 * 1024;
        size_t offset = 0;
        while (offset < nca_buf.size()) {
            const size_t chunk = std::min(nca_buf.size() - offset, kWriteChunkSize);
            rc = ncmContentStorageWritePlaceHolder(&stage.cs,
                                                   &stage.ph_id,
                                                   static_cast<s64>(offset),
                                                   nca_buf.data() + offset,
                                                   chunk);
            if (R_FAILED(rc)) {
                util::logLine("ncm: stage WritePlaceHolder failed, rc=" + formatHex32(rc));
                stage.reset();
                return false;
            }
            offset += chunk;
        }

        rc = ncmContentStorageFlushPlaceHolder(&stage.cs);
        if (R_FAILED(rc)) {
            util::logLine("ncm: stage FlushPlaceHolder failed, rc=" + formatHex32(rc));
            stage.reset();
            return false;
        }

        rc = ncmContentStorageGetPlaceHolderPath(&stage.cs, out_path, out_path_size, &stage.ph_id);
        if (R_FAILED(rc)) {
            util::logLine("ncm: stage GetPlaceHolderPath failed, rc=" + formatHex32(rc));
            stage.reset();
            return false;
        }

        util::logLine("ncm: staged USB CNMT NCA (" + std::to_string(nca_buf.size()) + " bytes) on " +
                      (use_native_sd ? "native SD" : "NAND") + ": " + std::string(out_path));
        return true;
    };

    NativeStageGuard stage;
    char content_path[FS_MAX_PATH] = {};
    if (!stageToNative(stage, false, content_path, sizeof(content_path)) &&
        !stageToNative(stage, true, content_path, sizeof(content_path))) {
        return false;
    }

    const bool ok = readCnmtFileFromFsPath(content_path, out_buf);
    stage.reset();
    return ok;
}

void NcmInstaller::rollbackRegistered() {
    if (!initialized_ || registered_.empty()) {
        registered_.clear();
        return;
    }
    util::logLine("ncm: rollback " + std::to_string(registered_.size()) + " registered contents");
    for (const auto& id : registered_) {
        ncmContentStorageDelete(&content_storage_, &id);
    }
    registered_.clear();
}

void NcmInstaller::cleanup() {
    if (!initialized_) return;

    util::logLine("ncm: cleanup " + std::to_string(placeholders_.size()) + " placeholders");

    for (const auto& id : placeholders_) {
        NcmPlaceHolderId pid;
        std::memcpy(&pid, &id, sizeof(NcmPlaceHolderId));
        ncmContentStorageDeletePlaceHolder(&content_storage_, &pid);
    }
    placeholders_.clear();
}

#endif // __SWITCH__

} // namespace installer
