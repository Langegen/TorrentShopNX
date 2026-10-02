#include "switch_utils.h"
#include "log.h"
#include <string>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <cmath>

#ifdef __SWITCH__
#include <switch.h>
#include <atomic>
#include <mutex>
extern std::recursive_mutex g_switch_service_mutex;
#endif

namespace util {

#ifdef __SWITCH__
namespace {
std::atomic<int> g_sleep_inhibit_count{0};
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

void preventSleepBegin() {
    int c = g_sleep_inhibit_count.fetch_add(1);
    if (c == 0) {
        if (hosversionAtLeast(5, 0, 0)) {
            appletSetAutoSleepDisabled(true);
        }
        appletSetMediaPlaybackState(true);
        util::logLine("switch_utils: sleep inhibited (auto-sleep disabled & keep-awake active)");
    }
}

void preventSleepEnd() {
    int c = g_sleep_inhibit_count.fetch_sub(1);
    if (c <= 1) {
        g_sleep_inhibit_count.store(0);
        if (hosversionAtLeast(5, 0, 0)) {
            appletSetAutoSleepDisabled(false);
        }
        appletSetMediaPlaybackState(false);
        util::logLine("switch_utils: sleep inhibition released");
    }
}

void cpuBoostBegin() {
    preventSleepBegin();
    if (!boostAllowed()) return;
    int c = g_cpu_boost_count.fetch_add(1);
    if (c > 0) return;   // already boosted by another active transfer
    Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    if (R_SUCCEEDED(rc)) {
        util::logLine("switch_utils: CPU boost enabled (1785 MHz)");
    } else {
        g_cpu_boost_count.fetch_sub(1);   // failed: never raised the refcount
        util::logLine("switch_utils: CPU boost failed rc=" + std::to_string(rc));
    }
}

void cpuBoostEnd() {
    preventSleepEnd();
    if (!boostAllowed()) return;
    int c = g_cpu_boost_count.fetch_sub(1);
    if (c < 1) {
        g_cpu_boost_count.store(0);
        return;
    }
    if (c == 1) {
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
void preventSleepBegin() {}
void preventSleepEnd() {}
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

SdkVersion getConsoleSdkVersion() {
    static SdkVersion cached_ver;
    static bool cached = false;
    if (cached) return cached_ver;

#ifdef __SWITCH__
    std::lock_guard<std::recursive_mutex> lock(g_switch_service_mutex);
    Result rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw = {};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            cached_ver.major = fw.major;
            cached_ver.minor = fw.minor;
            cached_ver.micro = fw.micro;
            cached_ver.valid = true;
            util::logLine("switch_utils: detected console firmware/SDK: " + cached_ver.toString() + " (" + std::string(fw.display_version) + ")");
        }
        setsysExit();
    }
    if (!cached_ver.valid) {
        u32 hos = hosversionGet();
        if (hos > 0) {
            cached_ver.major = HOSVER_MAJOR(hos);
            cached_ver.minor = HOSVER_MINOR(hos);
            cached_ver.micro = HOSVER_MICRO(hos);
            cached_ver.valid = true;
            util::logLine("switch_utils: fallback console SDK from hosversion: " + cached_ver.toString());
        }
    }
#else
    // Mock console SDK version for PC development
    cached_ver.major = 18;
    cached_ver.minor = 1;
    cached_ver.micro = 0;
    cached_ver.valid = true;
#endif

    cached = true;
    return cached_ver;
}

double parseNszCompressionRatio(const std::string& image_format, uint64_t total_torrent_bytes) {
    if (image_format.empty()) {
        return 1.30;
    }

    std::string lower = image_format;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    double ratio_from_pct = 0.0;
    double ratio_from_installed = 0.0;

    // 1. Search for percentage (e.g. "сжато ~46%", "сжато 46 %", "compressed ~50%")
    size_t pct_pos = image_format.find('%');
    if (pct_pos != std::string::npos && pct_pos > 0) {
        size_t i = pct_pos;
        while (i > 0 && (std::isdigit(static_cast<unsigned char>(image_format[i - 1])) ||
                         image_format[i - 1] == ' ' ||
                         image_format[i - 1] == '~')) {
            --i;
        }
        std::string digits;
        for (size_t k = i; k < pct_pos; ++k) {
            if (std::isdigit(static_cast<unsigned char>(image_format[k]))) {
                digits += image_format[k];
            }
        }
        if (!digits.empty()) {
            try {
                int pct = std::stoi(digits);
                if (pct >= 5 && pct <= 95) {
                    double r = 1.0 / (1.0 - (static_cast<double>(pct) / 100.0));
                    if (r >= 1.05 && r <= 6.0) {
                        ratio_from_pct = r;
                    }
                }
            } catch (...) {}
        }
    }

    // 2. Search for installed volume (e.g. "0.67 ГБ", "14.5 GB", "720 МБ")
    auto findUnit = [&](size_t& out_pos, uint64_t& out_multiplier) -> bool {
        size_t pos_gb = lower.find("gb");
        if (pos_gb != std::string::npos) {
            out_pos = pos_gb;
            out_multiplier = 1024ULL * 1024ULL * 1024ULL;
            return true;
        }
        size_t pos_mb = lower.find("mb");
        if (pos_mb != std::string::npos) {
            out_pos = pos_mb;
            out_multiplier = 1024ULL * 1024ULL;
            return true;
        }
        // UTF-8 Cyrillic ГБ / гб (\xD0\x93\xD0\x91 or \xD0\xB3\xD0\xB1)
        const std::string ru_gb_upper = "\xD0\x93\xD0\x91";
        const std::string ru_gb_lower = "\xD0\xB3\xD0\xB1";
        size_t pos_rugb = image_format.find(ru_gb_upper);
        if (pos_rugb == std::string::npos) pos_rugb = image_format.find(ru_gb_lower);
        if (pos_rugb != std::string::npos) {
            out_pos = pos_rugb;
            out_multiplier = 1024ULL * 1024ULL * 1024ULL;
            return true;
        }
        // UTF-8 Cyrillic МБ / мб (\xD0\x9C\xD0\x91 or \xD0\xBC\xD0\xB1)
        const std::string ru_mb_upper = "\xD0\x9C\xD0\x91";
        const std::string ru_mb_lower = "\xD0\xBC\xD0\xB1";
        size_t pos_rumb = image_format.find(ru_mb_upper);
        if (pos_rumb == std::string::npos) pos_rumb = image_format.find(ru_mb_lower);
        if (pos_rumb != std::string::npos) {
            out_pos = pos_rumb;
            out_multiplier = 1024ULL * 1024ULL;
            return true;
        }
        return false;
    };

    size_t unit_pos = std::string::npos;
    uint64_t multiplier = 0;
    if (findUnit(unit_pos, multiplier) && unit_pos > 0) {
        size_t end_num = unit_pos;
        while (end_num > 0 && image_format[end_num - 1] == ' ') {
            --end_num;
        }
        size_t start_num = end_num;
        while (start_num > 0) {
            char c = image_format[start_num - 1];
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == ',') {
                --start_num;
            } else {
                break;
            }
        }
        if (start_num < end_num) {
            std::string val_str = image_format.substr(start_num, end_num - start_num);
            std::replace(val_str.begin(), val_str.end(), ',', '.');
            try {
                double val = std::stod(val_str);
                uint64_t installed_bytes = static_cast<uint64_t>(val * static_cast<double>(multiplier));
                if (total_torrent_bytes > 0 && installed_bytes > 0) {
                    double r = static_cast<double>(installed_bytes) / static_cast<double>(total_torrent_bytes);
                    if (r >= 1.05 && r <= 6.0) {
                        ratio_from_installed = r;
                    }
                }
            } catch (...) {}
        }
    }

    if (ratio_from_installed > 0.0) {
        return ratio_from_installed;
    }
    if (ratio_from_pct > 0.0) {
        return ratio_from_pct;
    }

    if (lower.find("nsz") != std::string::npos || lower.find("xcz") != std::string::npos) {
        return 1.30;
    }
    return 1.0;
}

} // namespace util
