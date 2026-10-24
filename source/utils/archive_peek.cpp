#include "archive_peek.h"

#include "../datasource/i_data_source.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace util {

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kEocd64LocSig = 0x07064b50;
constexpr uint32_t kEocd64Sig = 0x06064b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint64_t kMaxCentralDir = 64ULL * 1024 * 1024;

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t le64(const uint8_t* p) {
    return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32);
}

std::string cleanEntryPath(std::string s, bool& isDir) {
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    isDir = !s.empty() && s.back() == '/';
    while (!s.empty() && s.back() == '/') s.pop_back();
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

// Reads exactly n bytes, waiting for the torrent to deliver them.
bool readFully(datasource::IDataSource* src, uint64_t off, void* buf, size_t n,
               const std::shared_ptr<std::atomic<bool>>& cancel) {
    auto* out = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < n) {
        if ((cancel && cancel->load()) || g_appExiting.load()) return false;
        size_t got = src->read(off + done, out + done, n - done);
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        done += got;
    }
    return true;
}

} // namespace

bool parseZipDirectory(uint64_t archive_size,
                       const std::function<bool(uint64_t, void*, size_t)>& read,
                       ArchivePeek& out,
                       std::string& out_error) {
    out = ArchivePeek();
    out.format = "zip";
    if (archive_size < 22) {
        out_error = "zip: file too small";
        return false;
    }

    // End of central directory: the last 22 bytes plus up to 64 KB of comment.
    const uint64_t tailLen = std::min<uint64_t>(archive_size, 22 + 65535 + 20);
    const uint64_t tailOff = archive_size - tailLen;
    std::vector<uint8_t> tail(static_cast<size_t>(tailLen));
    if (!read(tailOff, tail.data(), tail.size())) {
        out_error = "zip: cannot read the end of the archive";
        return false;
    }
    int64_t eocd = -1;
    for (int64_t i = static_cast<int64_t>(tail.size()) - 22; i >= 0; --i) {
        if (le32(&tail[static_cast<size_t>(i)]) == kEocdSig) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        out_error = "zip: end of central directory not found";
        return false;
    }
    const uint8_t* e = &tail[static_cast<size_t>(eocd)];
    if (le16(e + 4) != 0 || le16(e + 6) != 0) {
        out_error = "zip: spanned archive";
        return false;
    }
    uint64_t entries = le16(e + 10);
    uint64_t cdSize = le32(e + 12);
    uint64_t cdOff = le32(e + 16);

    if (entries == 0xFFFF || cdSize == 0xFFFFFFFFULL || cdOff == 0xFFFFFFFFULL) {
        // ZIP64: the locator sits right before the classic record.
        if (eocd < 20 || le32(&tail[static_cast<size_t>(eocd - 20)]) != kEocd64LocSig) {
            out_error = "zip: ZIP64 locator not found";
            return false;
        }
        uint64_t rec = le64(&tail[static_cast<size_t>(eocd - 20 + 8)]);
        uint8_t z[56];
        if (rec + sizeof(z) > archive_size || !read(rec, z, sizeof(z)) || le32(z) != kEocd64Sig) {
            out_error = "zip: bad ZIP64 record";
            return false;
        }
        entries = le64(z + 32);
        cdSize = le64(z + 40);
        cdOff = le64(z + 48);
    }
    if (cdSize > kMaxCentralDir || cdOff + cdSize > archive_size) {
        out_error = "zip: central directory out of range";
        return false;
    }

    std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
    if (cdSize > 0 && !read(cdOff, cd.data(), cd.size())) {
        out_error = "zip: cannot read the central directory";
        return false;
    }
    size_t p = 0;
    for (uint64_t n = 0; n < entries; ++n) {
        if (p + 46 > cd.size() || le32(&cd[p]) != kCentralSig) {
            out_error = "zip: broken central directory";
            return false;
        }
        const uint8_t* h = &cd[p];
        uint64_t usize = le32(h + 24);
        uint64_t csize = le32(h + 20);
        const uint16_t nameLen = le16(h + 28);
        const uint16_t extraLen = le16(h + 30);
        const uint16_t commentLen = le16(h + 32);
        if (p + 46 + nameLen + extraLen + commentLen > cd.size()) {
            out_error = "zip: truncated entry";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(h + 46), nameLen);
        // ZIP64 extra field holds the real sizes when the 32-bit ones are 0xFFFFFFFF.
        const uint8_t* x = h + 46 + nameLen;
        size_t xp = 0;
        while (xp + 4 <= extraLen) {
            uint16_t id = le16(x + xp);
            uint16_t len = le16(x + xp + 2);
            if (xp + 4 + len > extraLen) break;
            if (id == 0x0001) {
                size_t q = xp + 4;
                if (usize == 0xFFFFFFFFULL && q + 8 <= xp + 4 + len) { usize = le64(x + q); q += 8; }
                if (csize == 0xFFFFFFFFULL && q + 8 <= xp + 4 + len) { csize = le64(x + q); q += 8; }
            }
            xp += 4 + len;
        }
        p += 46 + nameLen + extraLen + commentLen;

        ArchiveEntryInfo info;
        info.path = cleanEntryPath(name, info.is_dir);
        if (info.path.empty()) continue;
        info.size = info.is_dir ? 0 : usize;
        out.total_uncompressed += info.size;
        out.entries.push_back(std::move(info));
    }
    return true;
}

bool peekArchiveFromSource(datasource::IDataSource* source,
                           uint64_t archive_size,
                           const std::shared_ptr<std::atomic<bool>>& cancel,
                           ArchivePeek& out,
                           std::string& out_error) {
    if (!source || archive_size < 6) {
        out_error = "peek: no data";
        return false;
    }
    uint8_t magic[6] = {0};
    if (!readFully(source, 0, magic, sizeof(magic), cancel)) {
        out_error = "peek: cannot read the archive header";
        return false;
    }
    static const uint8_t k7z[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    if (std::memcmp(magic, k7z, 6) == 0) {
        return peek7zArchiveFromSource(source, archive_size, cancel, out, out_error);
    }
    if (magic[0] == 'P' && magic[1] == 'K') {
        auto reader = [&](uint64_t off, void* buf, size_t n) {
            return readFully(source, off, buf, n, cancel);
        };
        return parseZipDirectory(archive_size, reader, out, out_error);
    }
    out_error = "peek: unsupported format";
    return false;
}

} // namespace util
