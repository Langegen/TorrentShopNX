#include "IgnoredUpdatesManager.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <unordered_set>
#include <borealis/extern/nlohmann/json.hpp>
#include "../utils/log.h"

namespace catalog {

void IgnoredUpdatesManager::init(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    filepath_ = path.empty() ? TSNX_IGNORED_UPDATES_PATH : path;
    load();
}

bool IgnoredUpdatesManager::load() {
    ignored_tids_.clear();

    if (filepath_.empty()) {
        filepath_ = TSNX_IGNORED_UPDATES_PATH;
    }

    std::ifstream in(filepath_);
    if (!in.is_open()) {
        util::logLine("ignored_updates: no file found at " + filepath_ + ", starting with empty list");
        return false;
    }

    try {
        nlohmann::json j;
        in >> j;
        if (j.is_array()) {
            for (const auto& item : j) {
                if (item.is_string()) {
                    std::string hexStr = item.get<std::string>();
                    if (!hexStr.empty()) {
                        try {
                            uint64_t tid = std::stoull(hexStr, nullptr, 16);
                            if (tid != 0) {
                                ignored_tids_.insert(tid);
                            }
                        } catch (...) {}
                    }
                } else if (item.is_number_unsigned()) {
                    uint64_t tid = item.get<uint64_t>();
                    if (tid != 0) {
                        ignored_tids_.insert(tid);
                    }
                }
            }
        }
        util::logLine("ignored_updates: loaded count=" + std::to_string(ignored_tids_.size()));
        return true;
    } catch (const std::exception& e) {
        util::logLine(std::string("ignored_updates: failed to parse JSON from ") + filepath_ + ": " + e.what());
        return false;
    } catch (...) {
        util::logLine("ignored_updates: unknown error parsing JSON from " + filepath_);
        return false;
    }
}

bool IgnoredUpdatesManager::save() {
    if (filepath_.empty()) {
        filepath_ = TSNX_IGNORED_UPDATES_PATH;
    }

    try {
        std::filesystem::path p(filepath_);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }
    } catch (...) {}

    std::ofstream out(filepath_);
    if (!out.is_open()) {
        util::logLine("ignored_updates: could not open file for writing: " + filepath_);
        return false;
    }

    try {
        nlohmann::json j = nlohmann::json::array();
        for (uint64_t tid : ignored_tids_) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)tid);
            j.push_back(std::string(buf));
        }
        out << j.dump(4);
        util::logLine("ignored_updates: saved count=" + std::to_string(ignored_tids_.size()));
        return true;
    } catch (const std::exception& e) {
        util::logLine(std::string("ignored_updates: failed to save JSON to ") + filepath_ + ": " + e.what());
        return false;
    } catch (...) {
        util::logLine("ignored_updates: unknown error saving JSON to " + filepath_);
        return false;
    }
}

bool IgnoredUpdatesManager::isIgnored(uint64_t titleId) const {
    if (titleId == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return ignored_tids_.count(titleId) > 0;
}

void IgnoredUpdatesManager::setIgnored(uint64_t titleId, bool ignored) {
    if (titleId == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (ignored) {
        ignored_tids_.insert(titleId);
    } else {
        ignored_tids_.erase(titleId);
    }
    save();
}

bool IgnoredUpdatesManager::toggleIgnored(uint64_t titleId) {
    if (titleId == 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    bool nowIgnored = false;
    if (ignored_tids_.count(titleId) > 0) {
        ignored_tids_.erase(titleId);
        nowIgnored = false;
    } else {
        ignored_tids_.insert(titleId);
        nowIgnored = true;
    }
    save();
    return nowIgnored;
}

size_t IgnoredUpdatesManager::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ignored_tids_.size();
}

const std::unordered_set<uint64_t>& IgnoredUpdatesManager::getIgnoredTitleIds() const {
    return ignored_tids_;
}

} // namespace catalog
