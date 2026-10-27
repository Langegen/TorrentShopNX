#include "ReleaseChooser.hpp"

#include <borealis.hpp>
#include <memory>

#include "../catalog/game_stats_manager.h"
#include "../catalog/titledb.h"

using namespace brls::literals;

namespace ui {

void showReleaseChooser(const std::vector<Game>& releases, std::function<void(const Game&)> onPick) {
    if (releases.empty()) return;

    std::vector<std::string> labels;
    for (size_t i = 0; i < releases.size(); ++i) {
        const Game& g = releases[i];
        std::string label = (i == 0 ? "★ " : "") + cleanTitle(g.title);
        if (!g.size.empty()) label += "  •  " + g.size;
        if (g.update_version) {
            label += "  •  " + brls::getStr("app/library/update_label",
                                                std::to_string(titledb::updateOrdinal(g.update_version)));
        }
        catalog::GameStats st;
        if (catalog::GameStatsManager::instance().copySwitchStats(g.topic_id, st)) {
            if (st.registered_at.size() >= 10) {
                label += "  •  " + titledb::formatDate(st.registered_at.substr(0, 10));
            }
            label += "  •  " + brls::getStr("app/library/release_seeds", std::to_string(st.seeds));
        }
        labels.push_back(label);
    }

    auto list = std::make_shared<std::vector<Game>>(releases);
    // Dropdown calls dismissCb on B as well (with the initial row), so the
    // pick is taken from the selection callback only.
    auto chosen = std::make_shared<int>(-1);
    auto* dropdown = new brls::Dropdown(
        "app/library/choose_release"_i18n, labels,
        [chosen](int sel) { *chosen = sel; },
        0,
        [list, chosen, onPick](int) {
            if (*chosen >= 0 && *chosen < static_cast<int>(list->size()) && onPick) {
                onPick((*list)[static_cast<size_t>(*chosen)]);
            }
        });
    brls::Application::pushActivity(new brls::Activity(dropdown));
}

} // namespace ui
