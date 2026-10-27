#pragma once

#include <cstdint>
#include <string>

namespace util {

// On the Switch a file over 4 GB on a FAT32 card can only exist as a
// "concatenation file". Creating the file with the BigFile attribute before
// writing makes writes past 4 GB work; emulators read it as an ordinary file.
// Does nothing for smaller files, for existing files and on PC.
void prepareBigFile(const std::string& path, uint64_t expected_size);

} // namespace util
