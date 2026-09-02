#pragma once

#include <string>
#include <unordered_set>
#include <vector>
#include <cstdint>
#include <mutex>
#include "../utils/app_paths.h"

namespace catalog {

class IgnoredUpdatesManager {
public:
    static IgnoredUpdatesManager& instance() {
        static IgnoredUpdatesManager inst;
        return inst;
    }

    void init(const std::string& path = TSNX_IGNORED_UPDATES_PATH);
    bool load();
    bool save();

    bool isIgnored(uint64_t titleId) const;
    void setIgnored(uint64_t titleId, bool ignored);
    bool toggleIgnored(uint64_t titleId);
    size_t count() const;
    const std::unordered_set<uint64_t>& getIgnoredTitleIds() const;

private:
    IgnoredUpdatesManager() = default;

    mutable std::mutex mutex_;
    std::string filepath_;
    std::unordered_set<uint64_t> ignored_tids_;
};

} // namespace catalog
