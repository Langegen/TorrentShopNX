#include "switch_utils.h"
#include "log.h"
#include <string>
#include <chrono>

#ifdef __SWITCH__
#include <switch.h>
#include <atomic>
#include <mutex>
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
    s64 total_space = 0;
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

namespace {
std::atomic<bool> g_backlight_off{false};
} // namespace

void setBacklightOff(bool off) {
    if (!hosversionAtLeast(4, 0, 0)) return;
    if (g_backlight_off.load() == off) return;
    
    Result rc = appletSetLcdBacklightOffEnabled(off);
    if (R_SUCCEEDED(rc)) {
        g_backlight_off.store(off);
        util::logLine(std::string("switch_utils: screen backlight ") + (off ? "turned OFF" : "turned ON"));
    } else {
        util::logLine("switch_utils: appletSetLcdBacklightOffEnabled(" + std::to_string(off) + ") failed rc=" + std::to_string(rc));
    }
}

bool isBacklightOff() {
    return g_backlight_off.load();
}
#else
void cpuBoostBegin() {}
void cpuBoostEnd() {}

namespace {
bool g_mock_backlight_off = false;
} // namespace

void setBacklightOff(bool off) {
    if (g_mock_backlight_off != off) {
        g_mock_backlight_off = off;
        util::logLine(std::string("switch_utils (mock): screen backlight ") + (off ? "OFF" : "ON"));
    }
}

bool isBacklightOff() {
    return g_mock_backlight_off;
}
#endif

bool getStorageStats(int storageId, int64_t& out_free_space, int64_t& out_total_space) {
#ifdef __SWITCH__
    std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
    int cache_idx = (storageId == 1) ? 1 : 0;
    const auto now = std::chrono::steady_clock::now();
    if (g_space_cache[cache_idx].valid &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_space_cache[cache_idx].last_query).count() < 5000) {
        out_free_space = g_space_cache[cache_idx].free_space;
        out_total_space = g_space_cache[cache_idx].total_space;
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
    s64 total_space = 0;
    rc = ncmContentStorageGetFreeSpaceSize(&cs, &free_space);
    if (R_SUCCEEDED(rc)) {
        rc = ncmContentStorageGetTotalSpaceSize(&cs, &total_space);
    }
    if (R_FAILED(rc)) {
        util::logLine("switch_utils: ncmContentStorage space query failed, rc=" + std::to_string(rc));
    }

    ncmContentStorageClose(&cs);
    ncmExit();

    if (R_FAILED(rc)) {
        return false;
    }

    g_space_cache[cache_idx].free_space = free_space;
    g_space_cache[cache_idx].total_space = total_space;
    g_space_cache[cache_idx].last_query = now;
    g_space_cache[cache_idx].valid = true;

    out_free_space = free_space;
    out_total_space = total_space;
    return true;
#else
    // Mock space on host system (PC)
    if (storageId == 1) { // SD
        out_total_space = 64ULL * 1024 * 1024 * 1024; // 64 GB
        out_free_space  = 32ULL * 1024 * 1024 * 1024; // 32 GB free
    } else { // NAND
        out_total_space = 32ULL * 1024 * 1024 * 1024; // 32 GB
        out_free_space  = 16ULL * 1024 * 1024 * 1024; // 16 GB free
    }
    return true;
#endif
}

bool getStorageFreeSpace(int storageId, int64_t& out_free_space) {
    int64_t total = 0;
    return getStorageStats(storageId, out_free_space, total);
}

} // namespace util
