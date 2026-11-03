#pragma once

// Crash-safe file replacement shared by config, catalog and recipe saves.
// Header-only so the PC test builds pick it up without build changes.

#include <cstdio>
#include <string>

namespace util {

// Writes data so that a crash or power loss leaves either the old or the new
// file, never a truncated one: write <path>.tmp and check that it was flushed
// and closed, move the old file to <path>.bak, rename the temp file into
// place, then drop the backup. rename() onto an existing file fails on the
// Switch SD card and on Windows, hence the backup step.
inline bool atomicWriteFile(const std::string& path, const void* data, size_t len,
                            std::string* err = nullptr) {
    const std::string tmp = path + ".tmp";
    const std::string bak = path + ".bak";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        if (err) *err = "cannot open " + tmp;
        return false;
    }
    bool ok = len == 0 || std::fwrite(data, 1, len, f) == len;
    if (std::fflush(f) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;  // the final flush can fail on a full card
    if (!ok) {
        std::remove(tmp.c_str());
        if (err) *err = "write failed: " + tmp;
        return false;
    }
    std::remove(bak.c_str());
    const bool hadOld = std::rename(path.c_str(), bak.c_str()) == 0;
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        if (hadOld) std::rename(bak.c_str(), path.c_str());
        std::remove(tmp.c_str());
        if (err) *err = "rename failed: " + path;
        return false;
    }
    if (hadOld) std::remove(bak.c_str());
    return true;
}

inline bool atomicWriteFile(const std::string& path, const std::string& data,
                            std::string* err = nullptr) {
    return atomicWriteFile(path, data.data(), data.size(), err);
}

// Moves a finished temp file over path the same way (old file to .bak first).
inline bool atomicReplaceFile(const std::string& tmp, const std::string& path) {
    const std::string bak = path + ".bak";
    std::remove(bak.c_str());
    const bool hadOld = std::rename(path.c_str(), bak.c_str()) == 0;
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        if (hadOld) std::rename(bak.c_str(), path.c_str());
        return false;
    }
    if (hadOld) std::remove(bak.c_str());
    return true;
}

// Path to read: the file itself, or its .bak when a crash happened between
// the two renames above and only the backup is left.
inline std::string existingOrBackup(const std::string& path) {
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        return path;
    }
    const std::string bak = path + ".bak";
    if (FILE* f = std::fopen(bak.c_str(), "rb")) {
        std::fclose(f);
        return bak;
    }
    return path;
}

} // namespace util
