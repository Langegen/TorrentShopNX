#pragma once

// Small file helpers shared by the homebrew catalog and installer. They work
// with "sdmc:/" paths on the console and plain paths in pctest.

#include <cctype>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace homebrew {
namespace fsx {

inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

inline bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

inline bool dirExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool readFile(const std::string& path, std::string& out) {
    out.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[64 * 1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

inline int makeDir(const std::string& p) {
#if defined(_WIN32)
    return _mkdir(p.c_str());
#else
    return mkdir(p.c_str(), 0777);
#endif
}

inline int removeDir(const std::string& p) {
#if defined(_WIN32)
    return _rmdir(p.c_str());
#else
    return rmdir(p.c_str());
#endif
}

// mkdir -p. Calls created(path) for every folder it actually made.
template <typename Fn>
inline void ensureDir(const std::string& path, Fn created) {
    std::string cur;
    size_t pos = 0;
    const size_t colon = path.find(":/");
    if (colon != std::string::npos) {
        cur = path.substr(0, colon + 2);
        pos = colon + 2;
    } else if (!path.empty() && path[0] == '/') {
        cur = "/";
        pos = 1;
    }
    while (pos <= path.size()) {
        size_t next = path.find('/', pos);
        if (next == std::string::npos) next = path.size();
        const std::string part = path.substr(pos, next - pos);
        if (part == "." && cur.empty()) {
            cur = ".";
        } else if (!part.empty() && part != ".") {
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += part;
            if (!dirExists(cur) && makeDir(cur) == 0) created(cur);
        }
        pos = next + 1;
    }
}

inline void ensureDir(const std::string& path) {
    ensureDir(path, [](const std::string&) {});
}

inline std::string parentOf(const std::string& path) {
    const size_t s = path.rfind('/');
    return s == std::string::npos ? "" : path.substr(0, s);
}

}  // namespace fsx
}  // namespace homebrew
