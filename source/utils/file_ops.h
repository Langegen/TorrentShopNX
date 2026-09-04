#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <atomic>

namespace util {

struct FileItem {
    std::string name;
    std::string path;
    bool isDir = false;
    uint64_t size = 0;
    time_t modifiedTime = 0;
    bool isSelected = false;
};

enum class ClipboardOp {
    None,
    Copy,
    Cut
};

struct ClipboardData {
    ClipboardOp op = ClipboardOp::None;
    std::vector<std::string> paths;
};

// Returns default root browsing directory ("sdmc:/" on Switch, "./" or root on PC)
std::string getDefaultRootPath();

// Lists contents of directory, sorted (folders first, then alphabetically)
std::vector<FileItem> listFolder(const std::string& folderPath, std::string& outError);

// Deletes file or directory recursively
bool deletePathRecursive(const std::string& path, std::string& outError);

// Batch delete multiple paths recursively
bool deleteMultiplePaths(const std::vector<std::string>& paths, std::string& outError);

// Copies file or directory recursively
bool copyPathRecursive(
    const std::string& src,
    const std::string& dst,
    std::function<void(float, const std::string&)> progressCb,
    std::shared_ptr<std::atomic<bool>> cancelToken,
    std::string& outError
);

// Moves or renames file or directory
bool movePath(const std::string& src, const std::string& dst, std::string& outError);

// Creates directory (and parent directories if needed)
bool createFolder(const std::string& path, std::string& outError);

// Renames file or directory within same parent directory
bool renameItem(const std::string& oldPath, const std::string& newName, std::string& outError);

// Storage info for a given path
bool getStorageSpace(const std::string& path, uint64_t& outFreeBytes, uint64_t& outTotalBytes);

// Global clipboard instance
ClipboardData& getClipboard();
void clearClipboard();

// Format helper
std::string formatFileSize(uint64_t bytes);

// Returns true if path ends with .nsp, .nsz, .xci, or .xcz (case-insensitive)
bool isGamePackage(const std::string& path);

} // namespace util
