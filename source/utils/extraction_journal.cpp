#include "extraction_journal.h"
#include "file_ops.h"
#include "log.h"

#include <filesystem>

namespace util {

namespace {

std::filesystem::path toFsPath(const std::string& p) {
#ifdef _WIN32
    return std::filesystem::u8path(p);
#else
    return std::filesystem::path(p);
#endif
}

bool pathExists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(toFsPath(p), ec);
}

bool isRoot(const std::string& p) {
    if (p.empty() || p == "/") return true;
    if (p.back() == ':') return true;                                   // "sdmc:" / "C:"
    return p.size() >= 2 && p.back() == '/' && p[p.size() - 2] == ':';  // "sdmc:/" / "C:/"
}

std::string normalizeDir(const std::string& path) {
    std::string d = normalizeFsPath(path);
    while (d.size() > 1 && d.back() == '/' && !isRoot(d)) d.pop_back();
    return d;
}

std::string parentOf(const std::string& p) {
    size_t pos = p.rfind('/');
    if (pos == std::string::npos) return "";
    if (pos == 0) return "/";
    std::string parent = p.substr(0, pos);
    if (parent.back() == ':') parent += '/';
    return parent;
}

} // namespace

void ExtractionJournal::createDirectories(const std::string& dir) {
    const std::string d = normalizeDir(dir);
    if (isRoot(d) || knownDirs_.count(d)) return;

    // Walk up until an existing/known directory; everything below it is new.
    std::vector<std::string> missing;
    for (std::string cur = d; !cur.empty() && !isRoot(cur) && !knownDirs_.count(cur); cur = parentOf(cur)) {
        if (pathExists(cur)) {
            knownDirs_.insert(cur);
            break;
        }
        missing.push_back(cur);
    }

    if (!missing.empty()) {
        safeCreateDirectories(d);
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        knownDirs_.insert(*it);
        if (pathExists(*it)) dirs_.push_back(*it);
    }
}

void ExtractionJournal::noteFile(const std::string& path) {
    const std::string p = normalizeFsPath(path);
    if (fileSet_.count(p) || pathExists(p)) return;
    fileSet_.insert(p);
    files_.push_back(p);
}

void ExtractionJournal::rollback() {
    if (files_.empty() && dirs_.empty()) return;

    util::logLine("extraction_journal: rollback start, files=" + std::to_string(files_.size()) +
                  " dirs=" + std::to_string(dirs_.size()));

    size_t removedFiles = 0, removedDirs = 0, failed = 0;
    bool interrupted = false;
    std::error_code ec;

    for (auto it = files_.rbegin(); it != files_.rend(); ++it) {
        if (g_appExiting.load()) { interrupted = true; break; }
        if (std::filesystem::remove(toFsPath(*it), ec)) {
            removedFiles++;
        } else if (ec) {
            failed++;
        }
    }

    // Children were created after their parents, so reverse order removes the
    // deepest first. remove() refuses non-empty dirs, which keeps foreign files.
    if (!interrupted) {
        for (auto it = dirs_.rbegin(); it != dirs_.rend(); ++it) {
            if (g_appExiting.load()) { interrupted = true; break; }
            if (std::filesystem::remove(toFsPath(*it), ec)) removedDirs++;
        }
    }

    util::logLine("extraction_journal: rollback done, removed files=" + std::to_string(removedFiles) +
                  " dirs=" + std::to_string(removedDirs) + " failed=" + std::to_string(failed) +
                  (interrupted ? " (interrupted by app exit)" : ""));

    files_.clear();
    fileSet_.clear();
    dirs_.clear();
    knownDirs_.clear();
}

} // namespace util
