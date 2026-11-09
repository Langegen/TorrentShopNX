#pragma once

// Small ZIP reader on zlib for homebrew packages: stored and deflate entries,
// ZIP64, CRC-32 and size checks. Same code on the console and in pctest
// (libarchive is not available for the PC tests).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace util {

struct ZipEntry {
    std::string name;  // as stored, '\' turned into '/'
    uint16_t method = 0;
    uint16_t flags = 0;
    uint32_t crc = 0;
    uint64_t csize = 0;
    uint64_t usize = 0;
    uint64_t local_offset = 0;
    bool is_dir = false;
};

class ZipReader {
public:
    ZipReader() = default;
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool open(const std::string& path, std::string& err);
    void close();

    const std::vector<ZipEntry>& entries() const { return entries_; }
    // Exact name first, then case-insensitive (archives made on Windows).
    const ZipEntry* find(const std::string& name) const;

    // Writes the entry to dest (overwrites). progress gets bytes written so
    // far. False on cancel, unsupported method, encryption or bad CRC; dest is
    // removed then.
    bool extract(const ZipEntry& e, const std::string& dest, const std::atomic<bool>* cancel,
                 const std::function<void(uint64_t)>& progress, std::string& err);

private:
    bool readAt(uint64_t off, void* buf, size_t n);

    FILE* f_ = nullptr;
    uint64_t size_ = 0;
    std::vector<ZipEntry> entries_;
};

}  // namespace util
