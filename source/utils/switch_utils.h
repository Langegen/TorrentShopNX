#pragma once

#include <cstdint>

namespace util {

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

} // namespace util
