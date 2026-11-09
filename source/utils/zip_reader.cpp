#include "zip_reader.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace util {

namespace {

constexpr uint32_t kLocalSig = 0x04034b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kEocd64Sig = 0x06064b50;
constexpr uint32_t kEocd64LocSig = 0x07064b50;
constexpr uint64_t kMaxCentralDir = 64ull * 1024 * 1024;

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }

int seek64(FILE* f, uint64_t off) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<long long>(off), SEEK_SET);
#else
    return fseeko(f, static_cast<off_t>(off), SEEK_SET);
#endif
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

ZipReader::~ZipReader() { close(); }

void ZipReader::close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
    size_ = 0;
    entries_.clear();
}

bool ZipReader::readAt(uint64_t off, void* buf, size_t n) {
    if (off + n > size_ || seek64(f_, off) != 0) return false;
    return std::fread(buf, 1, n, f_) == n;
}

bool ZipReader::open(const std::string& path, std::string& err) {
    close();
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) {
        err = "zip: cannot open";
        return false;
    }
    std::fseek(f_, 0, SEEK_END);
#if defined(_WIN32)
    const long long end = _ftelli64(f_);
#else
    const long long end = static_cast<long long>(ftello(f_));
#endif
    size_ = end > 0 ? static_cast<uint64_t>(end) : 0;
    if (size_ < 22) {
        err = "zip: file too small";
        close();
        return false;
    }

    const uint64_t tail_len = std::min<uint64_t>(size_, 22 + 65535 + 20);
    std::vector<uint8_t> tail(static_cast<size_t>(tail_len));
    if (!readAt(size_ - tail_len, tail.data(), tail.size())) {
        err = "zip: cannot read the end";
        close();
        return false;
    }
    long long eocd = -1;
    for (long long i = static_cast<long long>(tail.size()) - 22; i >= 0; --i) {
        if (le32(&tail[static_cast<size_t>(i)]) == kEocdSig) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        err = "zip: end of central directory not found";
        close();
        return false;
    }
    const uint8_t* e = &tail[static_cast<size_t>(eocd)];
    uint64_t count = le16(e + 10);
    uint64_t cd_size = le32(e + 12);
    uint64_t cd_off = le32(e + 16);
    if (count == 0xFFFF || cd_size == 0xFFFFFFFFull || cd_off == 0xFFFFFFFFull) {
        if (eocd < 20 || le32(&tail[static_cast<size_t>(eocd - 20)]) != kEocd64LocSig) {
            err = "zip: ZIP64 locator not found";
            close();
            return false;
        }
        const uint64_t rec = le64(&tail[static_cast<size_t>(eocd - 20 + 8)]);
        uint8_t z[56];
        if (!readAt(rec, z, sizeof(z)) || le32(z) != kEocd64Sig) {
            err = "zip: bad ZIP64 record";
            close();
            return false;
        }
        count = le64(z + 32);
        cd_size = le64(z + 40);
        cd_off = le64(z + 48);
    }
    if (cd_size > kMaxCentralDir || cd_off + cd_size > size_) {
        err = "zip: central directory out of range";
        close();
        return false;
    }
    std::vector<uint8_t> cd(static_cast<size_t>(cd_size));
    if (!readAt(cd_off, cd.data(), cd.size())) {
        err = "zip: cannot read the central directory";
        close();
        return false;
    }

    size_t p = 0;
    for (uint64_t i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || le32(&cd[p]) != kCentralSig) {
            err = "zip: broken central directory";
            close();
            return false;
        }
        const uint8_t* h = &cd[p];
        ZipEntry ent;
        ent.flags = le16(h + 8);
        ent.method = le16(h + 10);
        ent.crc = le32(h + 16);
        ent.csize = le32(h + 20);
        ent.usize = le32(h + 24);
        const uint16_t name_len = le16(h + 28), extra_len = le16(h + 30), comment_len = le16(h + 32);
        ent.local_offset = le32(h + 42);
        if (p + 46 + name_len + extra_len + comment_len > cd.size()) {
            err = "zip: broken central directory";
            close();
            return false;
        }
        ent.name.assign(reinterpret_cast<const char*>(h + 46), name_len);
        std::replace(ent.name.begin(), ent.name.end(), '\\', '/');
        ent.is_dir = !ent.name.empty() && ent.name.back() == '/';

        // ZIP64 extra field: only the values that overflowed are present, in this order.
        const uint8_t* x = h + 46 + name_len;
        size_t xp = 0;
        while (xp + 4 <= extra_len) {
            const uint16_t id = le16(x + xp), len = le16(x + xp + 2);
            if (xp + 4 + len > extra_len) break;
            if (id == 0x0001) {
                size_t q = xp + 4;
                if (ent.usize == 0xFFFFFFFFull && q + 8 <= xp + 4 + len) { ent.usize = le64(x + q); q += 8; }
                if (ent.csize == 0xFFFFFFFFull && q + 8 <= xp + 4 + len) { ent.csize = le64(x + q); q += 8; }
                if (ent.local_offset == 0xFFFFFFFFull && q + 8 <= xp + 4 + len) ent.local_offset = le64(x + q);
            }
            xp += 4 + len;
        }
        entries_.push_back(std::move(ent));
        p += 46 + name_len + extra_len + comment_len;
    }
    return true;
}

const ZipEntry* ZipReader::find(const std::string& name) const {
    for (const auto& e : entries_)
        if (e.name == name) return &e;
    const std::string l = lower(name);
    for (const auto& e : entries_)
        if (lower(e.name) == l) return &e;
    return nullptr;
}

bool ZipReader::extract(const ZipEntry& e, const std::string& dest, const std::atomic<bool>* cancel,
                        const std::function<void(uint64_t)>& progress, std::string& err) {
    if (!f_) {
        err = "zip: not open";
        return false;
    }
    if (e.flags & 1) {
        err = "zip: encrypted entry " + e.name;
        return false;
    }
    if (e.method != 0 && e.method != 8) {
        err = "zip: unsupported method " + std::to_string(e.method) + " for " + e.name;
        return false;
    }
    uint8_t lh[30];
    if (!readAt(e.local_offset, lh, sizeof(lh)) || le32(lh) != kLocalSig) {
        err = "zip: bad local header for " + e.name;
        return false;
    }
    const uint64_t data_off = e.local_offset + 30 + le16(lh + 26) + le16(lh + 28);
    if (data_off + e.csize > size_) {
        err = "zip: truncated entry " + e.name;
        return false;
    }

    FILE* out = std::fopen(dest.c_str(), "wb");
    if (!out) {
        err = "cannot create " + dest;
        return false;
    }
    bool ok = true;
    uLong crc = crc32(0L, Z_NULL, 0);
    uint64_t written = 0;
    std::vector<uint8_t> in(64 * 1024), buf(128 * 1024);

    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    const bool deflate = e.method == 8;
    if (deflate && inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        err = "zip: inflate init failed";
        std::fclose(out);
        std::remove(dest.c_str());
        return false;
    }

    uint64_t left = e.csize;
    uint64_t pos = data_off;
    int zret = Z_OK;
    while (ok && left > 0) {
        if (cancel && cancel->load()) {
            err = "cancelled";
            ok = false;
            break;
        }
        const size_t n = static_cast<size_t>(std::min<uint64_t>(left, in.size()));
        if (!readAt(pos, in.data(), n)) {
            err = "zip: read failed in " + e.name;
            ok = false;
            break;
        }
        pos += n;
        left -= n;
        if (!deflate) {
            crc = crc32(crc, in.data(), static_cast<uInt>(n));
            if (std::fwrite(in.data(), 1, n, out) != n) ok = false;
            written += n;
        } else {
            zs.next_in = in.data();
            zs.avail_in = static_cast<uInt>(n);
            while (ok && zs.avail_in > 0 && zret != Z_STREAM_END) {
                zs.next_out = buf.data();
                zs.avail_out = static_cast<uInt>(buf.size());
                zret = inflate(&zs, Z_NO_FLUSH);
                if (zret != Z_OK && zret != Z_STREAM_END) {
                    err = "zip: corrupt data in " + e.name;
                    ok = false;
                    break;
                }
                const size_t got = buf.size() - zs.avail_out;
                crc = crc32(crc, buf.data(), static_cast<uInt>(got));
                if (got && std::fwrite(buf.data(), 1, got, out) != got) ok = false;
                written += got;
            }
        }
        if (ok && progress) progress(written);
    }
    if (deflate) inflateEnd(&zs);
    if (std::fclose(out) != 0) ok = false;
    if (ok && err.empty() && (written != e.usize || crc != e.crc)) {
        err = "zip: CRC or size mismatch in " + e.name;
        ok = false;
    }
    if (!ok) {
        if (err.empty()) err = "cannot write " + dest;
        std::remove(dest.c_str());
    }
    return ok;
}

}  // namespace util
