#include "switch_utils.h"
#include <string>
#include <chrono>

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

struct SpaceCache {
    std::chrono::steady_clock::time_point last_query;
    s64 free_space = 0;
    bool valid = false;
};
SpaceCache g_space_cache[2]; // 0: NAND (BuiltInUser), 1: SD (SdCard)
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
    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
    int cache_idx = (storageId == 1) ? 1 : 0;
    const auto now = std::chrono::steady_clock::now();
    if (g_space_cache[cache_idx].valid &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_space_cache[cache_idx].last_query).count() < 5000) {
        out_free_space = g_space_cache[cache_idx].free_space;
        return true;
    }

    NcmContentStorage cs = {};
    NcmStorageId target_id = (storageId == 1) ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;
    
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        util::logLine("switch_utils: ncmInitialize failed, rc=" + std::to_string(rc));
        return false;
    }
    
    rc = ncmOpenContentStorage(&cs, target_id);
    if (R_FAILED(rc)) {
        util::logLine("switch_utils: ncmOpenContentStorage failed for storage=" + std::to_string(storageId) + ", rc=" + std::to_string(rc));
        ncmExit();
        return false;
    }
    
    s64 free_space = 0;
    rc = ncmContentStorageGetFreeSpaceSize(&cs, &free_space);
    if (R_FAILED(rc)) {
        util::logLine("switch_utils: ncmContentStorageGetFreeSpaceSize failed, rc=" + std::to_string(rc));
    }
    
    ncmContentStorageClose(&cs);
    ncmExit();
    
    if (R_FAILED(rc)) {
        return false;
    }
    
    g_space_cache[cache_idx].free_space = free_space;
    g_space_cache[cache_idx].last_query = now;
    g_space_cache[cache_idx].valid = true;

    out_free_space = free_space;
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
