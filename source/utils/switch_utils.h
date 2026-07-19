#pragma once

#include <cstdint>

namespace util {

// Returns true if free space was successfully queried.
// storageId: 1 for SD card, 0 for NAND (built-in user storage).
// On non-Switch platforms, returns simulated values.
bool getStorageFreeSpace(int storageId, int64_t& out_free_space);

} // namespace util
