#include "ncm_installer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <limits>
#include "../utils/log.h"

#ifdef __SWITCH__
#include <mutex>
extern std::recursive_mutex g_switch_service_mutex;
#endif

namespace installer {

static bool endsWithInsensitive(const std::string& value, const std::string& suffix) {
    if (value.size() < suffix.size()) return false;
    auto from = value.end() - static_cast<std::ptrdiff_t>(suffix.size());
    for (size_t i = 0; i < suffix.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(from[i]);
        unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

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
#endif
}

#ifdef __SWITCH__

bool NcmInstaller::begin(NcmStorageId storage) {
    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
    if (initialized_) {
        util::logLine("ncm: already initialized, resetting...");
        cleanup();
    }

    storage_id_ = storage;

    // Инициализация сервиса ncm
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        util::logLine("ncm: ncmInitialize failed, rc=" + std::to_string(rc));
        return false;
    }

    // Открыть ContentStorage для выбранного хранилища
    rc = ncmOpenContentStorage(&content_storage_, storage_id_);
    if (R_FAILED(rc)) {
        util::logLine("ncm: ncmOpenContentStorage failed, rc=" + std::to_string(rc));
        ncmExit();
        return false;
    }
    storage_opened_ = true;

    // Открыть ContentMetaDatabase
    rc = ncmOpenContentMetaDatabase(&meta_db_, storage_id_);
    if (R_FAILED(rc)) {
        util::logLine("ncm: ncmOpenContentMetaDatabase failed, rc=" + std::to_string(rc));
        ncmContentStorageClose(&content_storage_);
        storage_opened_ = false;
        ncmExit();
        return false;
    }
    db_opened_ = true;

    initialized_ = true;
    util::logLine("ncm: initialized, storage=" + std::to_string(static_cast<int>(storage_id_)));
    return true;
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
                          std::to_string((size + 1024*1024*1024 - 1) / (1024*1024*1024)) + " GB, rc=" + std::to_string(rc) + ")");
        } else {
            util::logLine("ncm: createPlaceHolder failed, rc=" + std::to_string(rc));
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
        Result rc = ncmContentStorageWritePlaceHolder(&content_storage_,
                                                       &placeholder_id,
                                                       static_cast<s64>(current_offset),
                                                       ptr,
                                                       chunk);
        if (R_FAILED(rc)) {
            util::logLine("ncm: writePlaceHolder failed, offset="
                           + std::to_string(current_offset)
                           + " size=" + std::to_string(chunk)
                           + " rc=" + std::to_string(rc));
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

    // РЈРґР°Р»СЏРµРј СЃСѓС‰РµСЃС‚РІСѓСЋС‰РёР№ РєРѕРЅС‚РµРЅС‚, РµСЃР»Рё РµСЃС‚СЊ (РїРµСЂРµСѓСЃС‚Р°РЅРѕРІРєР°)
    ncmContentStorageDelete(&content_storage_, &id);

    // Р РµРіРёСЃС‚СЂРёСЂСѓРµРј РїР»РµР№СЃС…РѕР»РґРµСЂ РєР°Рє РіРѕС‚РѕРІС‹Р№ РєРѕРЅС‚РµРЅС‚
    Result rc = ncmContentStorageRegister(&content_storage_, &id, &placeholder_id);
    if (R_FAILED(rc)) {
        util::logLine("ncm: register failed, rc=" + std::to_string(rc));
        return false;
    }

    // РЈР±РёСЂР°РµРј РёР· СЃРїРёСЃРєР° РїР»РµР№СЃС…РѕР»РґРµСЂРѕРІ
    for (auto it = placeholders_.begin(); it != placeholders_.end(); ++it) {
        if (std::memcmp(&(*it), &id, sizeof(NcmContentId)) == 0) {
            placeholders_.erase(it);
            break;
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
        util::logLine("ncm: get size from content id failed, rc=" + std::to_string(rc));
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
    rc = ncmContentStorageReadContentIdFile(&content_storage_,
                                            out_buf.data(),
                                            out_buf.size(),
                                            &id,
                                            0);
    if (R_FAILED(rc)) {
        util::logLine("ncm: read content id file failed, rc=" + std::to_string(rc));
        out_buf.clear();
        return false;
    }

    return true;
}

bool NcmInstaller::readCnmtFromContentMetaFs(const NcmContentId& id, std::vector<uint8_t>& out_buf) {
    if (!initialized_) return false;

    char content_path[FS_MAX_PATH];
    if (!getStoragePath(id, content_path, sizeof(content_path))) {
        return false;
    }

    FsFileSystem cnmt_fs = {};
    Result rc = fsOpenFileSystemWithId(&cnmt_fs,
                                       0,
                                       FsFileSystemType_ContentMeta,
                                       content_path,
                                       FsContentAttributes_None);
    if (R_FAILED(rc)) {
        util::logLine("ncm: fsOpenFileSystemWithId(ContentMeta) failed, rc=" + std::to_string(rc));
        return false;
    }

    FsDir dir = {};
    rc = fsFsOpenDirectory(&cnmt_fs, "/", FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) {
        util::logLine("ncm: fsFsOpenDirectory failed for content meta fs, rc=" + std::to_string(rc));
        fsFsClose(&cnmt_fs);
        return false;
    }

    std::string cnmt_name;
    while (cnmt_name.empty()) {
        FsDirectoryEntry entries[8] = {};
        s64 read_count = 0;
        rc = fsDirRead(&dir, &read_count, 8, entries);
        if (R_FAILED(rc)) {
            util::logLine("ncm: fsDirRead failed for content meta fs, rc=" + std::to_string(rc));
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
        util::logLine("ncm: fsFsOpenFile failed for " + cnmt_path + ", rc=" + std::to_string(rc));
        fsFsClose(&cnmt_fs);
        return false;
    }

    s64 cnmt_size = 0;
    rc = fsFileGetSize(&cnmt_file, &cnmt_size);
    if (R_FAILED(rc) || cnmt_size <= 0) {
        util::logLine("ncm: fsFileGetSize failed for cnmt, rc=" + std::to_string(rc));
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
        util::logLine("ncm: fsFileRead failed for cnmt, rc=" + std::to_string(rc));
        out_buf.clear();
        return false;
    }

    return true;
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
