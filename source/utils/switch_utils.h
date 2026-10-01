#pragma once

#include <cstdint>
#include <string>
#include <cstdio>

namespace util {

struct SdkVersion {
    uint8_t major = 0;
    uint8_t minor = 0;
    uint8_t micro = 0;
    uint8_t rel = 0;
    bool valid = false;

    std::string toString() const {
        if (!valid) return "";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u", major, minor, micro);
        return std::string(buf);
    }

    bool empty() const {
        return !valid || (major == 0 && minor == 0 && micro == 0);
    }

    bool operator>(const SdkVersion& o) const {
        if (!valid || !o.valid) return false;
        if (major != o.major) return major > o.major;
        if (minor != o.minor) return minor > o.minor;
        return micro > o.micro;
    }

    bool operator<(const SdkVersion& o) const {
        if (!valid || !o.valid) return false;
        if (major != o.major) return major < o.major;
        if (minor != o.minor) return minor < o.minor;
        return micro < o.micro;
    }

    bool operator>=(const SdkVersion& o) const {
        return !(*this < o);
    }

    bool operator<=(const SdkVersion& o) const {
        return !(*this > o);
    }

    bool operator==(const SdkVersion& o) const {
        return valid && o.valid && major == o.major && minor == o.minor && micro == o.micro;
    }

    static SdkVersion parse(uint32_t val) {
        SdkVersion v;
        if (val == 0) return v;
        uint8_t b3 = (val >> 24) & 0xFF;
        uint8_t b2 = (val >> 16) & 0xFF;
        uint8_t b1 = (val >> 8) & 0xFF;
        uint8_t b0 = val & 0xFF;
        if (b3 > 0) {
            v.major = b3;
            v.minor = b2;
            v.micro = b1;
            v.rel = b0;
        } else {
            // Pre-launch or small SDK format: 0.b2.b1
            v.major = b3;
            v.minor = b2;
            v.micro = b1;
            v.rel = b0;
        }
        v.valid = true;
        return v;
    }

    static SdkVersion parseString(const std::string& str) {
        SdkVersion v;
        if (str.empty()) return v;
        int maj = 0, min = 0, mic = 0;
        if (std::sscanf(str.c_str(), "%d.%d.%d", &maj, &min, &mic) >= 2) {
            v.major = static_cast<uint8_t>(maj);
            v.minor = static_cast<uint8_t>(min);
            v.micro = static_cast<uint8_t>(mic);
            v.valid = true;
        }
        return v;
    }
};

// Returns console firmware / SDK version (e.g. 18.1.0).
SdkVersion getConsoleSdkVersion();

// Returns true if free space was successfully queried.
// storageId: 1 for SD card, 0 for NAND (built-in user storage).
// On non-Switch platforms, returns simulated values.
bool getStorageFreeSpace(int storageId, int64_t& out_free_space);

// Returns both free and total space of a storage (SD or NAND).
// storageId: 1 for SD card, 0 for NAND (built-in user storage).
// On non-Switch platforms, returns simulated values.
bool getStorageStats(int storageId, int64_t& out_free_space, int64_t& out_total_space);

// Refcounted sleep inhibitor: disables auto-sleep (appletSetAutoSleepDisabled)
// and asserts media playback keep-awake (appletSetMediaPlaybackState) so the
// console screen does not dim or enter sleep during long background work.
// Works in both Title mode and Applet mode, safe and idempotent.
void preventSleepBegin();
void preventSleepEnd();

// CPU boost during heavy transfers: raises the CPU from 1020 MHz to 1785 MHz
// (ApmCpuBoostMode_FastLoad) and disables auto-sleep while at least one
// download/install/extraction is active. ~75% more CPU for SHA/zstd/bsd-IPC-bound work.
// Refcounted, idempotent, and a no-op off-Switch / in applet mode / on old firmware.
void cpuBoostBegin();
void cpuBoostEnd();

// RAII helper to ensure auto-sleep is disabled during a critical scope.
struct ScopedSleepInhibitor {
    ScopedSleepInhibitor() { preventSleepBegin(); }
    ~ScopedSleepInhibitor() { preventSleepEnd(); }
    ScopedSleepInhibitor(const ScopedSleepInhibitor&) = delete;
    ScopedSleepInhibitor& operator=(const ScopedSleepInhibitor&) = delete;
};

// RAII helper to ensure CPU boost and sleep prevention during a heavy operation.
struct ScopedCpuBoost {
    ScopedCpuBoost() { cpuBoostBegin(); }
    ~ScopedCpuBoost() { cpuBoostEnd(); }
    ScopedCpuBoost(const ScopedCpuBoost&) = delete;
    ScopedCpuBoost& operator=(const ScopedCpuBoost&) = delete;
};

// Screen backlight control (OLED/LCD display power saving & burn-in protection).
// off = true: turn screen backlight off (black screen).
// off = false: turn screen backlight back on.
// Safe, idempotent, and a no-op on non-Switch platforms.
void setBacklightOff(bool off);
bool isBacklightOff();

// Safely unmount RomFS and update tracking state
void unmountRomfs();

// Parses compression ratio for NSZ from image_format string (e.g. ".NSZ (сжато ~46%, установленный объём 0.67 ГБ)")
// or returns 1.30 as fallback.
double parseNszCompressionRatio(const std::string& image_format, uint64_t total_torrent_bytes = 0);

} // namespace util
