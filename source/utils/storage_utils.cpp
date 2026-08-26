#include "storage_utils.h"

#include <filesystem>
#include <mutex>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
extern std::recursive_mutex g_switch_service_mutex;
#endif

namespace util {

uint64_t dirSizeRecursive(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return 0;
    uint64_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             path, std::filesystem::directory_options::skip_permission_denied, ec)) {
        std::error_code ec2;
        if (entry.is_regular_file(ec2)) {
            total += entry.file_size(ec2);
        }
    }
    return total;
}

uint64_t pathSize(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return 0;
    if (std::filesystem::is_regular_file(path, ec)) {
        std::error_code ec2;
        std::uintmax_t sz = std::filesystem::file_size(path, ec2);
        return ec2 ? 0 : static_cast<uint64_t>(sz);
    }
    return dirSizeRecursive(path);
}

uint64_t deleteDirContents(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return 0;
    uint64_t freed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(
             path, std::filesystem::directory_options::skip_permission_denied, ec)) {
        std::error_code ec2;
        if (entry.is_regular_file(ec2)) {
            freed += entry.file_size(ec2);
        }
        std::filesystem::remove_all(entry.path(), ec2);
    }
    return freed;
}

uint64_t deleteFile(const std::string& path) {
    std::error_code ec;
    uint64_t size = 0;
    if (std::filesystem::exists(path, ec)) {
        std::error_code ec2;
        size = std::filesystem::file_size(path, ec2);
        if (ec2) size = 0;
        std::filesystem::remove(path, ec2);
    }
    return size;
}

#ifdef __SWITCH__
namespace {

bool listAllPlaceholders(NcmContentStorage* cs, std::vector<NcmPlaceHolderId>& out) {
    s32 offset = 0;
    while (true) {
        std::vector<NcmPlaceHolderId> batch(64);
        s32 count = 0;
        Result rc = ncmContentStorageListPlaceHolder(cs, batch.data(), 64, &count);
        if (R_FAILED(rc)) return false;
        for (s32 i = 0; i < count; ++i) {
            out.push_back(batch[i]);
        }
        if (count < 64) break;
        offset += count;
    }
    return true;
}

} // namespace
#endif

bool getLeftoverPlaceholders(int storageId, int& out_count, int64_t& out_total_size) {
    out_count = 0;
    out_total_size = 0;
#ifdef __SWITCH__
    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);

    NcmContentStorage cs = {};
    NcmStorageId target_id = (storageId == 1) ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;

    Result rc = ncmInitialize();
    if (R_FAILED(rc)) return false;
    rc = ncmOpenContentStorage(&cs, target_id);
    if (R_FAILED(rc)) {
        ncmExit();
        return false;
    }

    std::vector<NcmPlaceHolderId> ids;
    bool ok = listAllPlaceholders(&cs, ids);
    if (ok) {
        for (const auto& id : ids) {
            s64 sz = 0;
            if (R_SUCCEEDED(ncmContentStorageGetSizeFromPlaceHolderId(&cs, &sz, &id))) {
                out_total_size += sz;
            }
        }
        out_count = static_cast<int>(ids.size());
    }

    ncmContentStorageClose(&cs);
    ncmExit();
    return ok;
#else
    return true; // на хосте плейсхолдеров нет
#endif
}

bool cleanupLeftoverPlaceholders(int storageId, int& out_count, int64_t& out_freed_size) {
    out_count = 0;
    out_freed_size = 0;
#ifdef __SWITCH__
    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);

    NcmContentStorage cs = {};
    NcmStorageId target_id = (storageId == 1) ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;

    Result rc = ncmInitialize();
    if (R_FAILED(rc)) return false;
    rc = ncmOpenContentStorage(&cs, target_id);
    if (R_FAILED(rc)) {
        ncmExit();
        return false;
    }

    std::vector<NcmPlaceHolderId> ids;
    bool ok = listAllPlaceholders(&cs, ids);
    if (ok) {
        for (const auto& id : ids) {
            s64 sz = 0;
            if (R_SUCCEEDED(ncmContentStorageGetSizeFromPlaceHolderId(&cs, &sz, &id))) {
                out_freed_size += sz;
            }
        }
        out_count = static_cast<int>(ids.size());
        if (out_count > 0) {
            if (R_FAILED(ncmContentStorageCleanupAllPlaceHolder(&cs))) {
                ok = false;
            }
        }
    }

    ncmContentStorageClose(&cs);
    ncmExit();
    return ok;
#else
    return true;
#endif
}

} // namespace util