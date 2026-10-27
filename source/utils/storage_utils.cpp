#include "storage_utils.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
extern std::recursive_mutex g_switch_service_mutex;
#endif

namespace util {

#ifdef __SWITCH__
// Sums file sizes with the native FS API: fsDirRead returns up to kBatch
// entries per IPC and every entry already carries its size. The portable
// version below does one stat IPC per file; on a cache with thousands of
// covers that floods the fs service, which runs on the system core next to
// the compositor, and the UI thread then waits in eglSwapBuffers.
// Returns false when the path can't be handled natively (caller falls back).
static bool dirSizeNative(const std::string& path, uint64_t& out_total) {
    FsFileSystem* fs = nullptr;
    char root[FS_MAX_PATH] = {0};
    if (fsdevTranslatePath(path.c_str(), &fs, root) < 0 || !fs) return false;

    constexpr size_t kBatch = 64;
    std::vector<FsDirectoryEntry> entries(kBatch);
    std::vector<std::string> pending{root};
    uint64_t total = 0;
    bool openedRoot = false;

    while (!pending.empty()) {
        std::string dir = std::move(pending.back());
        pending.pop_back();

        FsDir d;
        if (R_FAILED(fsFsOpenDirectory(fs, dir.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d))) {
            if (!openedRoot) return false;   // root itself unreadable: let the caller decide
            continue;
        }
        openedRoot = true;

        for (;;) {
            s64 got = 0;
            if (R_FAILED(fsDirRead(&d, &got, kBatch, entries.data())) || got <= 0) break;
            for (s64 i = 0; i < got; ++i) {
                const FsDirectoryEntry& e = entries[static_cast<size_t>(i)];
                if (e.type == FsDirEntryType_Dir) {
                    std::string sub = dir;
                    if (sub.empty() || sub.back() != '/') sub += '/';
                    sub += e.name;
                    pending.push_back(std::move(sub));
                } else if (e.file_size > 0) {
                    total += static_cast<uint64_t>(e.file_size);
                }
            }
        }
        fsDirClose(&d);
    }

    out_total = total;
    return true;
}
#endif

uint64_t dirSizeRecursive(const std::string& path) {
#ifdef __SWITCH__
    {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return 0;
        uint64_t native = 0;
        if (dirSizeNative(path, native)) return native;
    }
#endif
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