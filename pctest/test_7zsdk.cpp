// PC smoke test for the vendored 7-Zip SDK extraction wrapper.
// Usage: test_7zsdk.exe <archive.7z> <refDir> <outDir>
// Extracts <archive.7z> into <outDir> and byte-compares the tree with <refDir>.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <atomic>
#include <memory>
#include <algorithm>

#include "../source/utils/sevenzip_utils.h"
#include "../source/utils/file_ops.h"

namespace fs = std::filesystem;

static int s_fail = 0;

static void fail(const std::string& msg) {
    fprintf(stderr, "FAIL: %s\n", msg.c_str());
    s_fail++;
}

static void collectFiles(const fs::path& dir, std::vector<std::string>& out) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        std::string rel = fs::relative(e.path(), dir).generic_string();
        out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
}

static bool readFileBytes(const std::string& path, std::vector<char>& out) {
#ifdef _WIN32
    std::wstring wpath = fs::u8path(path).native();
    FILE* f = _wfopen(wpath.c_str(), L"rb");
#else
    FILE* f = fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(sz > 0 ? (size_t)sz : 0);
    if (sz > 0) {
        size_t rd = fread(out.data(), 1, (size_t)sz, f);
        if (rd != (size_t)sz) { fclose(f); return false; }
    }
    fclose(f);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <archive.7z> <refDir> <outDir>\n", argv[0]);
        return 2;
    }
    std::string archive = argv[1];
    std::string refDir = argv[2];
    std::string outDir = argv[3];

    std::error_code ec;
    fs::remove_all(outDir, ec);
    fs::create_directories(outDir, ec);

    util::ArchiveProgress last;
    auto token = std::make_shared<std::atomic<bool>>(false);
    std::string err;
    bool ok = util::extract7zArchive(
        archive, outDir,
        [&last](const util::ArchiveProgress& p) { last = p; },
        token, err);

    printf("extract: %s (err=%s)\n", ok ? "OK" : "FAILED", err.c_str());
    if (!ok) { fail("extract7zArchive returned false: " + err); return 1; }

    std::vector<std::string> refFiles, outFiles;
    collectFiles(refDir, refFiles);
    collectFiles(outDir, outFiles);

    if (refFiles != outFiles) {
        fail("file lists differ");
        for (const auto& f : refFiles) {
            if (std::find(outFiles.begin(), outFiles.end(), f) == outFiles.end())
                printf("  missing in output: %s\n", f.c_str());
        }
        for (const auto& f : outFiles) {
            if (std::find(refFiles.begin(), refFiles.end(), f) == refFiles.end())
                printf("  extra in output: %s\n", f.c_str());
        }
        return 1;
    }

    for (const auto& f : refFiles) {
        std::vector<char> a, b;
        if (!readFileBytes(refDir + "/" + f, a)) { fail("cannot read ref " + f); continue; }
        if (!readFileBytes(outDir + "/" + f, b)) { fail("cannot read out " + f); continue; }
        if (a != b) {
            fail("content mismatch: " + f);
        }
    }

    if (s_fail == 0) {
        printf("ALL OK (%zu files, entries=%zu, percent=%.1f)\n",
               refFiles.size(), last.entriesProcessed, last.percentage);
        return 0;
    }
    return 1;
}
