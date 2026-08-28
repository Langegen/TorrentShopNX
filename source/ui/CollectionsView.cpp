#include "CollectionsView.hpp"
#include "CatalogView.hpp"
#include "CollectionGamesView.hpp"
#include "FavoritesView.hpp"
#include "FavoritesManager.hpp"
#include "../GameData.hpp"
#include "../utils/log.h"

#include <borealis/extern/nlohmann/json.hpp>
#include <fstream>
#include <algorithm>

extern std::vector<Game> g_games;

namespace ui {

namespace {

int cachedCount(const catalog::CollectionInfo& info) {
    std::string body;
    if (!readFileFast(catalog::CollectionsManager::cachePathFor(info.id), body)) return -1;
    try {
        auto j = nlohmann::json::parse(body);
        if (j.is_array()) return static_cast<int>(j.size());
    } catch (...) {}
    return -1;
}

brls::Box* createSectionDivider(const std::string& title_text) {
    brls::Box* headerBox = new brls::Box();
    headerBox->setFocusable(false); // Never grab focus so D-Pad skips cleanly to adjacent row
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setWidthPercentage(100.0f);
    headerBox->setMarginTop(12.0f);
    headerBox->setMarginBottom(10.0f);

    brls::Box* bar = new brls::Box();
    bar->setWidth(4.0f);
    bar->setHeight(18.0f);
    bar->setCornerRadius(2.0f);
    bar->setBackgroundColor(nvgRGBA(0, 224, 165, 255)); // Emerald
    bar->setMarginRight(8.0f);
    headerBox->addView(bar);

    brls::Label* lbl = new brls::Label();
    lbl->setText(title_text);
    lbl->setFontSize(14.0f);
    lbl->setTextColor(nvgRGBA(0, 224, 165, 240)); // Emerald
    headerBox->addView(lbl);

    return headerBox;
}

NVGcolor getGenreColor(const std::string& id) {
    if (id == "all_catalog")         return nvgRGBA(0, 224, 165, 255);   // Emerald
    if (id == "favorites")           return nvgRGBA(255, 193, 7, 255);   // Gold
    if (id == "new_release")         return nvgRGBA(0, 229, 255, 255);   // Cyan
    if (id == "top_100")             return nvgRGBA(255, 215, 0, 255);   // Golden Trophy
    if (id == "action_adventure")    return nvgRGBA(255, 87, 34, 255);   // Orange-Red
    if (id == "arcade")              return nvgRGBA(171, 71, 188, 255);  // Purple
    if (id == "horror")              return nvgRGBA(239, 83, 80, 255);   // Crimson
    if (id == "metroidvania")        return nvgRGBA(33, 150, 243, 255);  // Electric Blue
    if (id == "party_multiplayer")   return nvgRGBA(76, 175, 80, 255);   // Green
    if (id == "platformers")         return nvgRGBA(255, 167, 38, 255);  // Amber
    if (id == "puzzles")             return nvgRGBA(38, 198, 218, 255);  // Teal
    if (id == "roguelike_roguelite") return nvgRGBA(141, 110, 99, 255);  // Warm Bronze
    if (id == "rpg_jrpg")            return nvgRGBA(236, 64, 122, 255);  // Rose
    if (id == "shooters")            return nvgRGBA(229, 57, 53, 255);   // Red
    if (id == "simulation_cozy")     return nvgRGBA(139, 195, 74, 255);  // Lime
    if (id == "strategy_tactics")    return nvgRGBA(120, 144, 156, 255); // Steel
    if (id == "visual_novels")       return nvgRGBA(186, 104, 200, 255); // Violet
    return nvgRGBA(0, 224, 165, 255);
}

} // namespace

CollectionsView::CollectionsView()
    : alive_flag_(std::make_shared<std::atomic<bool>>(true)) {
}

CollectionsView::~CollectionsView() {
    *alive_flag_ = false;
}

void CollectionsView::onContentAvailable() {
    if (scroll) {
        scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    }

    rebuildGrid();

    // Pre-warm collection caches in background and update counters
    const auto& collections = catalog::CollectionsManager::instance().collections();
    for (const auto& info : collections) {
        std::shared_ptr<std::atomic<bool>> flag = alive_flag_;
        catalog::CollectionInfo cInfo = info;
        auto it = collection_cards_.find(info.id);
        CollectionCard* card = (it != collection_cards_.end()) ? it->second : nullptr;

        brls::async([flag, card, cInfo]() {
            std::vector<catalog::CollectionEntry> entries;
            bool from_cache = true;
            bool ok = catalog::CollectionsManager::instance().loadCollection(cInfo, entries, from_cache);
            if (!ok || !flag->load()) return;

            size_t count = entries.size();
            brls::sync([flag, card, count, cInfo]() {
                if (flag->load() && card) {
                    card->setCountText(std::to_string(count) + " игр");
                }
            });
        });
    }
}

void CollectionsView::rebuildGrid() {
    if (!listBox) return;
    listBox->clearViews();
    collection_cards_.clear();
    grid_.clear();

    const auto& collections = catalog::CollectionsManager::instance().collections();

    // Find "new_release" and "top_100" collections
    catalog::CollectionInfo newReleaseInfo{"new_release", "Новые релизы", "Свежие игры и новинки"};
    catalog::CollectionInfo top100Info{"top_100", "Топ-100", "Лучшие игры по Metacritic"};
    for (const auto& c : collections) {
        if (c.id == "new_release") newReleaseInfo = c;
        if (c.id == "top_100") top100Info = c;
    }

    // ============================================================
    // SECTION 1: ОСНОВНЫЕ РАЗДЕЛЫ (Row 0: 4 cards)
    // ============================================================
    listBox->addView(createSectionDivider("ОСНОВНЫЕ РАЗДЕЛЫ"));

    brls::Box* mainRow = new brls::Box();
    mainRow->setAxis(brls::Axis::ROW);
    mainRow->setJustifyContent(brls::JustifyContent::FLEX_START);
    mainRow->setWidthPercentage(100.0f);
    mainRow->setMarginBottom(12.0f);

    std::vector<CollectionCard*> row0;

    // 1.1 Весь каталог
    {
        std::string totalCount = std::to_string(g_games.size()) + " игр";
        CollectionCard* cardAll = new CollectionCard(
            "all_catalog",
            "Весь каталог",
            "Полная база доступных игр",
            totalCount,
            getGenreColor("all_catalog"),
            []() { brls::Application::pushActivity(new CatalogView()); }
        );
        cardAll->setMarginRight(16.0f);
        mainRow->addView(cardAll);
        row0.push_back(cardAll);
    }

    // 1.2 Избранное
    {
        size_t favCount = catalog::FavoritesManager::instance().getFavorites().size();
        CollectionCard* cardFav = new CollectionCard(
            "favorites",
            "Избранное",
            "Ваши сохранённые игры",
            std::to_string(favCount) + " игр",
            getGenreColor("favorites"),
            []() { brls::Application::pushActivity(new FavoritesView()); }
        );
        cardFav->setMarginRight(16.0f);
        mainRow->addView(cardFav);
        row0.push_back(cardFav);
    }

    // 1.3 Новые релизы
    {
        int nrCount = cachedCount(newReleaseInfo);
        std::string nrText = (nrCount >= 0) ? (std::to_string(nrCount) + " новинок") : "Свежие игры";
        CollectionCard* cardNew = new CollectionCard(
            "new_release",
            newReleaseInfo.getName(),
            newReleaseInfo.getDescription(),
            nrText,
            getGenreColor("new_release"),
            [newReleaseInfo]() { brls::Application::pushActivity(new CollectionGamesView(newReleaseInfo)); }
        );
        cardNew->setMarginRight(16.0f);
        collection_cards_[newReleaseInfo.id] = cardNew;
        mainRow->addView(cardNew);
        row0.push_back(cardNew);
    }

    // 1.4 Топ-100
    {
        int topCount = cachedCount(top100Info);
        std::string topText = (topCount >= 0) ? (std::to_string(topCount) + " игр") : "★ Топ-100";
        CollectionCard* cardTop = new CollectionCard(
            "top_100",
            top100Info.getName(),
            top100Info.getDescription(),
            topText,
            getGenreColor("top_100"),
            [top100Info]() { brls::Application::pushActivity(new CollectionGamesView(top100Info)); }
        );
        collection_cards_[top100Info.id] = cardTop;
        mainRow->addView(cardTop);
        row0.push_back(cardTop);
    }

    listBox->addView(mainRow);
    grid_.push_back(row0);

    // ============================================================
    // SECTION 2: ЖАНРОВЫЕ И ТЕМАТИЧЕСКИЕ ПОДБОРКИ (4xN Grid)
    // ============================================================
    listBox->addView(createSectionDivider("ЖАНРОВЫЕ И ТЕМАТИЧЕСКИЕ ПОДБОРКИ"));

    brls::Box* currentRow = nullptr;
    std::vector<CollectionCard*> currentGridRow;

    for (const auto& info : collections) {
        // Skip "new_release" and "top_100" as they are in Section 1
        if (info.id == "new_release" || info.id == "top_100") continue;

        if (currentGridRow.empty()) {
            currentRow = new brls::Box();
            currentRow->setAxis(brls::Axis::ROW);
            currentRow->setJustifyContent(brls::JustifyContent::FLEX_START);
            currentRow->setWidthPercentage(100.0f);
            currentRow->setMarginBottom(12.0f);
            listBox->addView(currentRow);
        }

        int cached = cachedCount(info);
        std::string countStr = (cached >= 0) ? (std::to_string(cached) + " игр") : "Подборка";

        CollectionCard* card = new CollectionCard(
            info.id,
            info.getName(),
            info.getDescription(),
            countStr,
            getGenreColor(info.id),
            [info]() { brls::Application::pushActivity(new CollectionGamesView(info)); }
        );
        if (currentGridRow.size() < 3) {
            card->setMarginRight(16.0f);
        }
        collection_cards_[info.id] = card;
        currentRow->addView(card);
        currentGridRow.push_back(card);

        if (currentGridRow.size() == 4) {
            grid_.push_back(currentGridRow);
            currentGridRow.clear();
            currentRow = nullptr;
        }
    }

    // Handle last row if it has fewer than 4 items
    if (!currentGridRow.empty() && currentRow) {
        grid_.push_back(currentGridRow);
    }

    util::logLine("CollectionsView: successfully built 4xN grid with " +
                  std::to_string(grid_.size()) + " rows");
}

} // namespace ui
