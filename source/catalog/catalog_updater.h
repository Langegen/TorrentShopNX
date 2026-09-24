#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include "../GameData.hpp"
#include <borealis/extern/nlohmann/json.hpp>

namespace ui {
class CatalogProgressNotification;
}

namespace catalog {

enum class CatalogUpdateMode {
    Auto,      // Scheduled: checks 48h for full or 4h for diff
    ForceDiff, // Forced delta update
    ForceFull  // Forced full catalog re-download
};

struct CatalogUpdateResult {
    bool success = false;
    bool already_up_to_date = false;
    bool was_full = false;
    int added_count = 0;
    int updated_count = 0;
    int deleted_count = 0;
    std::string message;
};

class CatalogUpdater {
public:
    static CatalogUpdater& instance();

    bool isUpdateRunning() const;

    // Checks scheduled timers and runs update if due (called on MainMenu startup)
    void checkAndRunScheduledUpdate();

    // Starts an update with the specified mode and optional completion callback
    void startUpdate(CatalogUpdateMode mode,
                     std::function<void(const CatalogUpdateResult&)> onDone = nullptr,
                     bool showNotification = true);

    // Applies diff payload (from catalog_diff.json) to in-memory games vector
    static bool applyDiffToGames(std::vector<Game>& currentGames,
                                 const nlohmann::json& diffJson,
                                 const std::string& langKey,
                                 int& outAdded,
                                 int& outUpdated,
                                 int& outDeleted);

private:
    CatalogUpdater() = default;
    ~CatalogUpdater() = default;

    CatalogUpdateResult performDiffUpdate(const std::string& diffUrl,
                                          const std::string& langKey,
                                          const std::shared_ptr<bool>& aliveToken,
                                          ui::CatalogProgressNotification* notif);

    CatalogUpdateResult performFullUpdate(const std::string& catalogUrl,
                                          const std::shared_ptr<bool>& aliveToken,
                                          ui::CatalogProgressNotification* notif);
};

} // namespace catalog
