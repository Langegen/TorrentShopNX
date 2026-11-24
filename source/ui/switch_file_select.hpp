#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ui {

struct SwitchFileCandidate {
    uint64_t tid = 0;
    uint32_t version = 0;
    bool is_game = false;
    bool too_large = false;
    bool installed = false;
};

/* Auto-selects Switch files cleanly:
 * - In no-install mode, selects all Switch game files that fit the FAT32 limit.
 * - In install mode, deduplicates files sharing the same Title ID so only the
 *   highest version per Title ID is eligible for automatic selection, and
 *   leaves that Title ID unselected if its highest version is already installed. */
inline std::vector<bool> selectLatestSwitchFiles(const std::vector<SwitchFileCandidate>& items,
                                                 bool no_install) {
    std::vector<bool> sel(items.size(), false);
    if (items.empty()) return sel;

    if (no_install) {
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].is_game && !items[i].too_large) {
                sel[i] = true;
            }
        }
        return sel;
    }

    std::unordered_map<uint64_t, size_t> best_by_tid;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& c = items[i];
        if (!c.is_game || c.too_large) continue;
        if (c.tid == 0) {
            if (!c.installed) sel[i] = true;
            continue;
        }
        auto it = best_by_tid.find(c.tid);
        if (it == best_by_tid.end() || c.version > items[it->second].version) {
            best_by_tid[c.tid] = i;
        }
    }

    for (const auto& kv : best_by_tid) {
        size_t idx = kv.second;
        if (!items[idx].installed) {
            sel[idx] = true;
        }
    }
    return sel;
}

} // namespace ui
