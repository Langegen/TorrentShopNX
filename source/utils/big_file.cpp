#include "big_file.h"

#include "app_paths.h"
#include "log.h"

#include <filesystem>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace util {

void prepareBigFile(const std::string& path, uint64_t expected_size) {
#ifdef __SWITCH__
    constexpr uint64_t kFat32Limit = 0xFFFFFFFFULL;
    if (expected_size <= kFat32Limit) return;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return;
    const std::string prefix = "sdmc:";
    if (path.rfind(prefix, 0) != 0) return;
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs) return;
    tsnx_ensure_parent_dirs(path.c_str());
    char rel[FS_MAX_PATH] = {0};
    std::snprintf(rel, sizeof(rel), "%s", path.c_str() + prefix.size());
    Result rc = fsFsCreateFile(fs, rel, 0, FsCreateOption_BigFile);
    if (R_FAILED(rc)) {
        util::logLine("big_file: could not create " + path + " as a big file (rc=" + std::to_string(rc) + ")");
    } else {
        util::logLine("big_file: created " + path + " as a big file for " + std::to_string(expected_size) + " bytes");
    }
#else
    (void)path;
    (void)expected_size;
#endif
}

} // namespace util
