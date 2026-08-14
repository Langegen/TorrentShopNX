#pragma once

#include <cstdint>

namespace util {

// Returns true if free space was successfully queried.
// storageId: 1 for SD card, 0 for NAND (built-in user storage).
// On non-Switch platforms, returns simulated values.
bool getStorageFreeSpace(int storageId, int64_t& out_free_space);

// CPU boost during heavy transfers: raises the CPU from 1020 MHz to 1785 MHz
// (ApmCpuBoostMode_FastLoad) and disables auto-sleep while at least one
// download/install is active. ~75% more CPU for SHA/zstd/bsd-IPC-bound work,
// which is what the pipensx measurements found on real hardware. Refcounted,
// idempotent, and a no-op off-Switch / in applet mode / on old firmware.
void cpuBoostBegin();
void cpuBoostEnd();

} // namespace util
