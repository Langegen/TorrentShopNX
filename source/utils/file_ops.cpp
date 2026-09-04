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
    std::error_code ec;
    std::filesystem::path p(path);
    if (!std::filesystem::exists(p, ec)) {
        return true; // Already gone
    }

    std::uintmax_t removed = std::filesystem::remove_all(p, ec);
    if (ec) {
        outError = ec.message();
        return false;
    }
    (void)removed;
    return true;
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

bool copyPathRecursive(
    const std::string& src,
    const std::string& dst,
    std::function<void(float, const std::string&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError
) {
    std::error_code ec;
    std::filesystem::path srcP(src);
    std::filesystem::path dstP(dst);

    if (!std::filesystem::exists(srcP, ec)) {
        outError = "Source does not exist: " + src;
        return false;
    }

    try {
        if (std::filesystem::is_directory(srcP, ec)) {
            std::filesystem::create_directories(dstP, ec);
            for (const auto& entry : std::filesystem::recursive_directory_iterator(srcP, ec)) {
                if (cancelToken && cancelToken->load()) {
                    outError = "Operation cancelled";
                    return false;
                }

                auto rel = std::filesystem::relative(entry.path(), srcP, ec);
                std::filesystem::path target = dstP / rel;

                if (entry.is_directory(ec)) {
                    std::filesystem::create_directories(target, ec);
                } else if (entry.is_regular_file(ec)) {
                    if (progressCb) {
                        progressCb(0.0f, entry.path().filename().generic_string());
                    }
                    std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing, ec);
                    if (ec) {
                        outError = "Copy error: " + ec.message();
                        return false;
                    }
                }
            }
        } else {
            if (progressCb) {
                progressCb(0.0f, srcP.filename().generic_string());
            }
            if (dstP.has_parent_path()) {
                std::filesystem::create_directories(dstP.parent_path(), ec);
            }
            std::filesystem::copy_file(srcP, dstP, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                outError = ec.message();
                return false;
            }
        }
    } catch (const std::exception& e) {
        outError = e.what();
        return false;
    }

    return true;
}

bool movePath(const std::string& src, const std::string& dst, std::string& outError) {
    std::error_code ec;
    std::filesystem::path srcP(src);
    std::filesystem::path dstP(dst);

    if (!std::filesystem::exists(srcP, ec)) {
        outError = "Source does not exist: " + src;
        return false;
    }

    if (dstP.has_parent_path()) {
        std::filesystem::create_directories(dstP.parent_path(), ec);
    }

    std::filesystem::rename(srcP, dstP, ec);
    if (ec) {
        // If rename across filesystems failed, fallback to copy + remove
        std::string copyErr;
        if (copyPathRecursive(src, dst, nullptr, nullptr, copyErr)) {
            std::filesystem::remove_all(srcP, ec);
            return true;
        }
        outError = ec.message() + " (" + copyErr + ")";
        return false;
    }
    return true;
}

bool createFolder(const std::string& path, std::string& outError) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        outError = ec.message();
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
