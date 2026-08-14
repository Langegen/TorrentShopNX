#include "switch_utils.h"
#include <string>

#ifdef __SWITCH__
#include <switch.h>
#include <atomic>
#include <mutex>
#include "../utils/log.h"
extern std::recursive_mutex g_switch_service_mutex;
#endif

namespace util {

#ifdef __SWITCH__
namespace {
std::atomic<int> g_cpu_boost_count{0};

bool boostAllowed() {
    AppletType type = appletGetAppletType();
    if (type == AppletType_LibraryApplet || type == AppletType_OverlayApplet)
        return false;   // no apm privileges / no point in applet mode
    return hosversionAtLeast(7, 0, 0);   // ApmCpuBoostMode needs 7.0.0+
}
} // namespace

void cpuBoostBegin() {
    if (!boostAllowed()) return;
    int c = g_cpu_boost_count.fetch_add(1);
    if (c > 0) return;   // already boosted by another active transfer
    Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    if (R_SUCCEEDED(rc) && hosversionAtLeast(5, 0, 0)) {
        appletSetAutoSleepDisabled(true);
        util::logLine("switch_utils: CPU boost enabled (1785 MHz), auto-sleep disabled");
    } else {
        g_cpu_boost_count.fetch_sub(1);   // failed: never raised the refcount
        util::logLine("switch_utils: CPU boost failed rc=" + std::to_string(rc));
    }
}

void cpuBoostEnd() {
    if (!boostAllowed()) return;
    int c = g_cpu_boost_count.fetch_sub(1);
    if (c < 1) {
        g_cpu_boost_count.store(0);
        return;
    }
    if (c == 1) {
        appletSetAutoSleepDisabled(false);
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        util::logLine("switch_utils: CPU boost released");
    }
}
#else
void cpuBoostBegin() {}
void cpuBoostEnd() {}
#endif

bool getStorageFreeSpace(int storageId, int64_t& out_free_space) {
#ifdef __SWITCH__
    util::logLine("switch_utils: getStorageFreeSpace start, storageId=" + std::to_string(storageId));
    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
    NcmContentStorage cs = {};
    NcmStorageId target_id = (storageId == 1) ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;
    
    util::logLine("switch_utils: calling ncmInitialize...");
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        util::logLine("switch_utils: ncmInitialize failed, rc=" + std::to_string(rc));
        return false;
    }
    
    util::logLine("switch_utils: calling ncmOpenContentStorage...");
    rc = ncmOpenContentStorage(&cs, target_id);
    if (R_FAILED(rc)) {
        util::logLine("switch_utils: ncmOpenContentStorage failed for storage=" + std::to_string(storageId) + ", rc=" + std::to_string(rc));
        ncmExit();
        return false;
    }
    
    util::logLine("switch_utils: calling ncmContentStorageGetFreeSpaceSize...");
    s64 free_space = 0;
    rc = ncmContentStorageGetFreeSpaceSize(&cs, &free_space);
    if (R_FAILED(rc)) {
        util::logLine("switch_utils: ncmContentStorageGetFreeSpaceSize failed, rc=" + std::to_string(rc));
    } else {
        util::logLine("switch_utils: ncmContentStorageGetFreeSpaceSize success, free=" + std::to_string(free_space));
    }
    
    util::logLine("switch_utils: calling ncmContentStorageClose...");
    ncmContentStorageClose(&cs);
    
    util::logLine("switch_utils: calling ncmExit...");
    ncmExit();
    
    if (R_FAILED(rc)) {
        return false;
    }
    
    out_free_space = free_space;
    util::logLine("switch_utils: getStorageFreeSpace success, free_space=" + std::to_string(free_space));
    return true;
#else
    // Mock space on host system (PC)
    if (storageId == 1) { // SD
        out_free_space = 32ULL * 1024 * 1024 * 1024; // 32 GB
    } else { // NAND
        out_free_space = 16ULL * 1024 * 1024 * 1024; // 16 GB
    }
    return true;
#endif
}

} // namespace util
