#include "file_ops.h"
#include "log.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <chrono>

namespace util {

static ClipboardData s_clipboard;

std::string getDefaultRootPath() {
#ifdef __SWITCH__
    return "sdmc:/";
#else
    std::error_code ec;
    std::filesystem::path cur = std::filesystem::current_path(ec);
    if (!ec && !cur.empty()) {
        return cur.generic_string();
    }
    return ".";
#endif
}

static std::string toLower(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

std::vector<FileItem> listFolder(const std::string& folderPath, std::string& outError) {
    std::vector<FileItem> items;
    std::error_code ec;

    std::filesystem::path p(folderPath);
    if (!std::filesystem::exists(p, ec)) {
        outError = "Path does not exist: " + folderPath;
        return items;
    }
    if (!std::filesystem::is_directory(p, ec)) {
        outError = "Path is not a directory: " + folderPath;
        return items;
    }

    try {
        for (const auto& entry : std::filesystem::directory_iterator(p, std::filesystem::directory_options::skip_permission_denied, ec)) {
            FileItem item;
            item.name = entry.path().filename().generic_string();
            if (item.name.empty() || item.name == "." || item.name == "..") {
                continue;
            }
            item.path = entry.path().generic_string();
            item.isDir = entry.is_directory(ec);
            if (!item.isDir) {
                item.size = entry.file_size(ec);
                if (ec) item.size = 0;
            } else {
                item.size = 0;
            }

            auto ftime = entry.last_write_time(ec);
            if (!ec) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
                );
                item.modifiedTime = std::chrono::system_clock::to_time_t(sctp);
            } else {
                item.modifiedTime = 0;
            }

            items.push_back(item);
        }
    } catch (const std::exception& e) {
        outError = e.what();
    }

    // Sort: directories first, then alphabetical case-insensitive
    std::sort(items.begin(), items.end(), [](const FileItem& a, const FileItem& b) {
        if (a.isDir != b.isDir) {
            return a.isDir > b.isDir;
        }
        std::string lowerA = toLower(a.name);
        std::string lowerB = toLower(b.name);
        return lowerA < lowerB;
    });

    return items;
}

bool deletePathRecursive(const std::string& path, std::string& outError) {
    if (path.empty()) return true;

    std::error_code ec;
    std::filesystem::path p(path);
    if (!std::filesystem::exists(p, ec)) {
        return true; // Already gone
    }

    try {
        if (std::filesystem::is_directory(p, ec)) {
            for (auto it = std::filesystem::directory_iterator(p, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::directory_iterator(); ++it) {
                std::string subErr;
                if (!deletePathRecursive(it->path().generic_string(), subErr)) {
                    outError = subErr;
                    return false;
                }
            }
            if (!std::filesystem::remove(p, ec) || ec) {
                if (std::remove(path.c_str()) != 0) {
                    outError = ec ? ec.message() : "Failed to remove directory";
                    return false;
                }
            }
            return true;
        } else {
            bool ok = std::filesystem::remove(p, ec);
            if (!ok || ec) {
                if (std::remove(path.c_str()) != 0) {
                    outError = ec ? ec.message() : "Failed to remove file";
                    return false;
                }
            }
            return true;
        }
    } catch (const std::exception& e) {
        outError = e.what();
        return false;
    } catch (...) {
        outError = "Unknown error deleting path";
        return false;
    }
}

bool deleteMultiplePaths(const std::vector<std::string>& paths, std::string& outError) {
    bool allOk = true;
    std::string errs;
    for (const auto& path : paths) {
        std::string err;
        if (!deletePathRecursive(path, err)) {
            allOk = false;
            if (!errs.empty()) errs += "\n";
            errs += path + ": " + err;
        }
    }
    if (!allOk) {
        outError = errs;
    }
    return allOk;
}

bool safeCreateDirectories(const std::string& path) {
    if (path.empty()) return true;

    std::string norm = path;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    while (norm.size() > 1 && norm.back() == '/') {
        if (norm == "sdmc:/" || (norm.size() == 3 && norm[1] == ':')) break;
        norm.pop_back();
    }

    std::error_code ec;
    if (std::filesystem::exists(norm, ec)) {
        return true;
    }

    std::string cur;
    size_t pos = 0;
    if (norm.rfind("sdmc:/", 0) == 0) {
        cur = "sdmc:/";
        pos = 6;
    } else if (norm.size() >= 3 && norm[1] == ':' && norm[2] == '/') {
        cur = norm.substr(0, 3);
        pos = 3;
    } else if (norm.rfind("/", 0) == 0) {
        cur = "/";
        pos = 1;
    }

    while (pos < norm.size()) {
        size_t next = norm.find('/', pos);
        std::string part = (next == std::string::npos) ? norm.substr(pos) : norm.substr(pos, next - pos);
        if (!part.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += "/";
            cur += part;
            if (!std::filesystem::exists(cur, ec)) {
                std::filesystem::create_directory(cur, ec);
            }
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return true;
}

static bool copySingleFile(const std::string& src, const std::string& dst, std::string& outError) {
    size_t lastSlash = dst.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        safeCreateDirectories(dst.substr(0, lastSlash));
    }

    FILE* in = fopen(src.c_str(), "rb");
    if (!in) {
        outError = "Cannot open source file: " + src;
        return false;
    }

    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) {
        std::remove(dst.c_str());
        out = fopen(dst.c_str(), "wb");
    }
    if (!out) {
        fclose(in);
        outError = "Cannot create destination file: " + dst;
        return false;
    }

    constexpr size_t BUF_SZ = 64 * 1024;
    std::vector<char> buf(BUF_SZ);
    bool ok = true;

    while (size_t bytes = fread(buf.data(), 1, BUF_SZ, in)) {
        if (fwrite(buf.data(), 1, bytes, out) != bytes) {
            outError = "Write error to " + dst;
            ok = false;
            break;
        }
    }

    if (ok && ferror(in)) {
        outError = "Read error from " + src;
        ok = false;
    }

    fclose(in);
    fclose(out);

    if (!ok) {
        std::remove(dst.c_str());
    }
    return ok;
}

bool copyPathRecursive(
    const std::string& src,
    const std::string& dst,
    std::function<void(float, const std::string&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError
) {
    util::logLine("file_ops: copyPathRecursive start " + src + " -> " + dst);
    std::error_code ec;
    std::filesystem::path srcP(src);

    if (!std::filesystem::exists(srcP, ec)) {
        outError = "Source does not exist: " + src;
        util::logLine("file_ops: copyPathRecursive source does not exist: " + src);
        return false;
    }

    if (src == dst) {
        return true;
    }

    try {
        if (std::filesystem::is_directory(srcP, ec)) {
            std::string srcNorm = src;
            std::replace(srcNorm.begin(), srcNorm.end(), '\\', '/');
            if (srcNorm.back() != '/') srcNorm += '/';
            std::string dstNorm = dst;
            std::replace(dstNorm.begin(), dstNorm.end(), '\\', '/');
            if (dstNorm.back() != '/') dstNorm += '/';
            if (dstNorm.rfind(srcNorm, 0) == 0) {
                outError = "Cannot copy directory into itself";
                return false;
            }

            safeCreateDirectories(dst);

            std::string srcBase = src;
            std::replace(srcBase.begin(), srcBase.end(), '\\', '/');
            while (!srcBase.empty() && srcBase.back() == '/') srcBase.pop_back();

            for (const auto& entry : std::filesystem::recursive_directory_iterator(srcP, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (cancelToken && cancelToken->load()) {
                    outError = "Operation cancelled";
                    return false;
                }

                std::string entryPath = entry.path().generic_string();
                std::string rel = entryPath.substr(srcBase.length());
                while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) {
                    rel.erase(0, 1);
                }
                if (rel.empty()) continue;

                std::string target = dst;
                if (!target.empty() && target.back() != '/') target += '/';
                target += rel;

                if (entry.is_directory(ec)) {
                    safeCreateDirectories(target);
                } else if (entry.is_regular_file(ec)) {
                    if (progressCb) {
                        progressCb(0.0f, entry.path().filename().generic_string());
                    }
                    if (!copySingleFile(entryPath, target, outError)) {
                        util::logLine("file_ops: copySingleFile failed: " + entryPath + " -> " + target + ": " + outError);
                        return false;
                    }
                }
            }
        } else {
            if (progressCb) {
                progressCb(0.0f, srcP.filename().generic_string());
            }
            if (!copySingleFile(src, dst, outError)) {
                util::logLine("file_ops: copySingleFile failed: " + src + " -> " + dst + ": " + outError);
                return false;
            }
        }
    } catch (const std::exception& e) {
        outError = e.what();
        util::logLine("file_ops: exception in copyPathRecursive: " + std::string(e.what()));
        return false;
    }

    util::logLine("file_ops: copyPathRecursive completed successfully " + src + " -> " + dst);
    return true;
}

bool movePath(const std::string& src, const std::string& dst, std::string& outError) {
    util::logLine("file_ops: movePath start " + src + " -> " + dst);
    std::error_code ec;
    std::filesystem::path srcP(src);
    std::filesystem::path dstP(dst);

    if (!std::filesystem::exists(srcP, ec)) {
        outError = "Source does not exist: " + src;
        util::logLine("file_ops: movePath source does not exist: " + src);
        return false;
    }

    if (src == dst) {
        return true;
    }

    bool srcIsDir = std::filesystem::is_directory(srcP, ec);
    bool dstExists = std::filesystem::exists(dstP, ec);

    // If destination does NOT exist, try fast atomic rename first
    if (!dstExists) {
        size_t lastSlash = dst.find_last_of("/\\");
        if (lastSlash != std::string::npos) {
            safeCreateDirectories(dst.substr(0, lastSlash));
        }

        if (std::rename(src.c_str(), dst.c_str()) == 0) {
            util::logLine("file_ops: movePath fast rename succeeded");
            return true;
        }
    }

    // If src is a single file:
    if (!srcIsDir) {
        size_t lastSlash = dst.find_last_of("/\\");
        if (lastSlash != std::string::npos) {
            safeCreateDirectories(dst.substr(0, lastSlash));
        }
        std::remove(dst.c_str());
        if (std::rename(src.c_str(), dst.c_str()) == 0) {
            util::logLine("file_ops: movePath file rename succeeded");
            return true;
        }
        // Fallback: copy file and delete source
        if (copySingleFile(src, dst, outError)) {
            std::remove(src.c_str());
            return true;
        }
        return false;
    }

    // ── FOLDER MERGE ──
    // src is a directory, and dst either already exists as a directory or rename failed.
    std::string srcNorm = src;
    std::replace(srcNorm.begin(), srcNorm.end(), '\\', '/');
    if (srcNorm.back() != '/') srcNorm += '/';
    std::string dstNorm = dst;
    std::replace(dstNorm.begin(), dstNorm.end(), '\\', '/');
    if (dstNorm.back() != '/') dstNorm += '/';

    if (dstNorm.rfind(srcNorm, 0) == 0) {
        outError = "Cannot move directory into its own subdirectory";
        return false;
    }

    safeCreateDirectories(dst);

    std::string srcBase = src;
    std::replace(srcBase.begin(), srcBase.end(), '\\', '/');
    while (!srcBase.empty() && srcBase.back() == '/') srcBase.pop_back();

    util::logLine("file_ops: movePath starting directory merge: " + srcBase + " -> " + dst);

    // Iterate all items in source directory
    for (const auto& entry : std::filesystem::recursive_directory_iterator(srcP, std::filesystem::directory_options::skip_permission_denied, ec)) {
        std::string entryPath = entry.path().generic_string();
        std::string rel = entryPath.substr(srcBase.length());
        while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\')) {
            rel.erase(0, 1);
        }
        if (rel.empty()) continue;

        std::string target = dst;
        if (!target.empty() && target.back() != '/') target += '/';
        target += rel;

        if (entry.is_directory(ec)) {
            safeCreateDirectories(target);
        } else if (entry.is_regular_file(ec)) {
            size_t tSlash = target.find_last_of("/\\");
            if (tSlash != std::string::npos) {
                safeCreateDirectories(target.substr(0, tSlash));
            }
            std::remove(target.c_str());
            if (std::rename(entryPath.c_str(), target.c_str()) != 0) {
                if (!copySingleFile(entryPath, target, outError)) {
                    util::logLine("file_ops: movePath copySingleFile fallback failed: " + entryPath + " -> " + target);
                    return false;
                }
                std::remove(entryPath.c_str());
            }
        }
    }

    // Now delete the empty source directory tree
    std::string delErr;
    if (!deletePathRecursive(src, delErr)) {
        util::logLine("file_ops: movePath clean up source dir warning: " + delErr);
    }

    util::logLine("file_ops: movePath directory merge finished successfully");
    return true;
}

bool createFolder(const std::string& path, std::string& outError) {
    if (!safeCreateDirectories(path)) {
        outError = "Failed to create folder";
        return false;
    }
    return true;
}

bool renameItem(const std::string& oldPath, const std::string& newName, std::string& outError) {
    if (newName.empty()) {
        outError = "Name cannot be empty";
        return false;
    }
    std::error_code ec;
    std::filesystem::path oldP(oldPath);
    if (!std::filesystem::exists(oldP, ec)) {
        outError = "Path does not exist: " + oldPath;
        return false;
    }

    std::filesystem::path newP = oldP.parent_path() / newName;
    std::filesystem::rename(oldP, newP, ec);
    if (ec) {
        outError = ec.message();
        return false;
    }
    return true;
}

bool getStorageSpace(const std::string& path, uint64_t& outFreeBytes, uint64_t& outTotalBytes) {
    std::error_code ec;
    std::filesystem::space_info info = std::filesystem::space(path, ec);
    if (ec) {
        outFreeBytes = 0;
        outTotalBytes = 0;
        return false;
    }
    outFreeBytes = info.available;
    outTotalBytes = info.capacity;
    return true;
}

ClipboardData& getClipboard() {
    return s_clipboard;
}

void clearClipboard() {
    s_clipboard.op = ClipboardOp::None;
    s_clipboard.paths.clear();
}

std::string formatFileSize(uint64_t bytes) {
    double size = static_cast<double>(bytes);
    int unit = 0;
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    while (size >= 1024.0 && unit < 4) { size /= 1024.0; ++unit; }
    char buf[64];
    if (unit == 0) {
        std::snprintf(buf, sizeof(buf), "%llu %s", (unsigned long long)bytes, units[unit]);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit]);
    }
    return std::string(buf);
}

bool isGamePackage(const std::string& path) {
    if (path.size() < 4) return false;
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return (lower.rfind(".nsp") == lower.size() - 4 ||
            lower.rfind(".nsz") == lower.size() - 4 ||
            lower.rfind(".xci") == lower.size() - 4 ||
            lower.rfind(".xcz") == lower.size() - 4);
}

} // namespace util
