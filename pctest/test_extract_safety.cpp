// PC test for 7z codec pre-check, libarchive ARM64 support and rollback of
// partially extracted files.
// Usage: test_extract_safety <dir with big_arm64.7z, small_corrupt.7z, encrypted.7z, src_big/> <workDir>
// Archives are made by pctest/make_extract_safety_archives.py (7-Zip 23+).

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

#include "../source/datasource/i_data_source.h"
#include "../source/utils/archive_utils.h"
#include "../source/utils/log.h"
#include "../source/utils/switch_utils.h"

namespace fs = std::filesystem;

std::atomic<bool> g_appExiting{false};
namespace util {
void cpuBoostBegin() {}
void cpuBoostEnd() {}
}

static int s_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("  FAIL: %s\n", msg); s_fail++; } else { std::printf("  ok: %s\n", msg); } } while (0)

// File-backed stream that records how much of the archive was actually read,
// standing in for the torrent stream.
class RecordingSource : public datasource::IDataSource {
public:
    explicit RecordingSource(const std::string& path) : f_(std::fopen(path.c_str(), "rb")) {
        if (f_) { std::fseek(f_, 0, SEEK_END); size_ = (uint64_t)ftello64(f_); }
    }
    ~RecordingSource() override { if (f_) std::fclose(f_); }
    bool open(const std::string&, int) override { return f_ != nullptr; }
    size_t read(uint64_t offset, void* buf, size_t size) override {
        std::lock_guard<std::mutex> lk(m_);
        if (offset >= size_) return 0;
        size = (size_t)std::min<uint64_t>(size, size_ - offset);
        fseeko64(f_, (off64_t)offset, SEEK_SET);
        size_t n = std::fread(buf, 1, size, f_);
        bytesRead += n;
        return n;
    }
    uint64_t totalSize() const override { return size_; }
    bool isAvailable() const override { return f_ != nullptr; }
    datasource::SourceType type() const override { return datasource::SourceType::File; }
    void close() override {}
    uint64_t bytesRead = 0;
private:
    FILE* f_;
    uint64_t size_ = 0;
    std::mutex m_;
};

static std::vector<std::string> listTree(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    for (auto& e : fs::recursive_directory_iterator(dir)) out.push_back(fs::relative(e.path(), dir).generic_string());
    std::sort(out.begin(), out.end());
    return out;
}

static bool sameFile(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec)) return false;
    FILE* fa = std::fopen(a.string().c_str(), "rb");
    FILE* fb = std::fopen(b.string().c_str(), "rb");
    bool same = fa && fb;
    std::vector<char> ba(1 << 20), bb(1 << 20);
    while (same) {
        size_t na = std::fread(ba.data(), 1, ba.size(), fa);
        size_t nb = std::fread(bb.data(), 1, bb.size(), fb);
        if (na != nb || std::memcmp(ba.data(), bb.data(), na) != 0) same = false;
        if (na == 0) break;
    }
    if (fa) std::fclose(fa);
    if (fb) std::fclose(fb);
    return same;
}

// Destination with something that already existed: must survive any rollback.
static fs::path freshDest(const fs::path& work, const char* name) {
    fs::path d = work / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d / "switch" / "existing_app");
    FILE* f = std::fopen((d / "switch" / "existing_app" / "keep.txt").string().c_str(), "wb");
    std::fputs("keep", f);
    std::fclose(f);
    return d;
}

static const std::vector<std::string> kPreexisting = {"switch", "switch/existing_app", "switch/existing_app/keep.txt"};

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <archivesDir> <workDir>\n", argv[0]); return 2; }
    const fs::path in = argv[1], work = argv[2];
    fs::create_directories(work);

    std::printf("[1] libarchive probe\n");
    CHECK(util::libarchiveSupports7zArm64Lzma(), "linked libarchive decodes 7z LZMA2+ARM64");

    std::printf("[2] 80 MB LZMA2+ARM64 folder via stream -> libarchive path\n");
    {
        fs::path dest = freshDest(work, "big");
        RecordingSource src((in / "big_arm64.7z").string());
        std::string err;
        bool ok = util::extractArchiveFromSource(&src, src.totalSize(), dest.string(), nullptr,
                                                 std::make_shared<std::atomic<bool>>(false), nullptr, err);
        std::printf("  result=%d err='%s'\n", ok, err.c_str());
        CHECK(ok, "extraction succeeded");
        for (const char* f : {"switch/hl2_nx/hl2/payload.bin", "switch/hl2_nx/lib/libclient.so", "switch/hl2_nx/readme.txt"}) {
            CHECK(sameFile(in / "src_big" / f, dest / f), (std::string("content matches: ") + f).c_str());
        }
    }

    std::printf("[3] cancel mid-way on libarchive path -> rollback\n");
    {
        fs::path dest = freshDest(work, "cancel");
        RecordingSource src((in / "big_arm64.7z").string());
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        std::string err;
        bool ok = util::extractArchiveFromSource(&src, src.totalSize(), dest.string(),
            [&](const util::ArchiveProgress& p) { if (p.bytesExtracted > 20u * 1024 * 1024) cancel->store(true); },
            cancel, nullptr, err);
        std::printf("  result=%d err='%s'\n", ok, err.c_str());
        CHECK(!ok, "extraction reported failure");
        CHECK(listTree(dest) == kPreexisting, "only pre-existing files remain");
    }

    std::printf("[4] corrupt data on 7z SDK path -> rollback\n");
    {
        fs::path dest = freshDest(work, "corrupt");
        std::string err;
        bool ok = util::extractArchive((in / "small_corrupt.7z").string(), dest.string(), nullptr,
                                       std::make_shared<std::atomic<bool>>(false), err);
        std::printf("  result=%d err='%s'\n", ok, err.c_str());
        CHECK(!ok, "extraction reported failure");
        CHECK(listTree(dest) == kPreexisting, "only pre-existing files remain");
    }

    std::printf("[5] encrypted 7z via stream -> rejected from header only\n");
    {
        fs::path dest = freshDest(work, "enc");
        RecordingSource src((in / "encrypted.7z").string());
        std::string err;
        bool ok = util::extractArchiveFromSource(&src, src.totalSize(), dest.string(), nullptr,
                                                 std::make_shared<std::atomic<bool>>(false), nullptr, err);
        std::printf("  result=%d err='%s' read=%llu of %llu bytes\n", ok, err.c_str(),
                    (unsigned long long)src.bytesRead, (unsigned long long)src.totalSize());
        CHECK(!ok, "extraction reported failure");
        CHECK(err.find("зашифрован") != std::string::npos, "error says the archive is encrypted");
        CHECK(src.bytesRead < src.totalSize() / 4, "payload was not read (no libarchive fallback)");
        CHECK(listTree(dest) == kPreexisting, "nothing was created");
    }

    std::printf(s_fail ? "FAILED: %d check(s)\n" : "ALL OK\n", s_fail);
    return s_fail ? 1 : 0;
}
