#include "CollectionsView.hpp"
#include "CatalogView.hpp"
#include "CollectionGamesView.hpp"
#include "FavoritesView.hpp"
#include "FavoritesManager.hpp"
#include "../GameData.hpp"
#include "../utils/log.h"

#include <borealis/extern/nlohmann/json.hpp>

#include <fstream>

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

} // namespace

CollectionsView::CollectionsView() : alive_flag_(std::make_shared<std::atomic<bool>>(true)) {
}

CollectionsView::~CollectionsView() {
    *alive_flag_ = false;
}

void CollectionsView::onContentAvailable() {
    rebuildList();

    // Pre-warm collection caches in background and update counters when done
    const auto& collections = catalog::CollectionsManager::instance().collections();
    for (size_t i = 0; i < collections.size(); ++i) {
        std::shared_ptr<std::atomic<bool>> flag = alive_flag_;
        brls::Label* label = (i < countLabels_.size()) ? countLabels_[i] : nullptr;
        catalog::CollectionInfo info = collections[i];
        brls::async([flag, label, info]() {
            std::vector<catalog::CollectionEntry> entries;
            bool from_cache = true;
            bool ok = catalog::CollectionsManager::instance().loadCollection(info, entries, from_cache);
            if (!ok || !flag->load()) return;
            size_t count = entries.size();
            brls::sync([flag, label, count]() {
                if (flag->load() && label) {
                    label->setText(brls::getStr("app/collections/games_count", std::to_string(count)));
                }
            });
        });
    }
}

void CollectionsView::rebuildList() {
    listBox->clearViews();
    countLabels_.clear();

    // Row 0: full catalog
    {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(76);
        row->setWidth(brls::View::AUTO);
        row->setPadding(10, 12, 10, 12);
        row->setMarginBottom(8);
        row->setFocusable(true);

        auto* col = new brls::Box();
        col->setAxis(brls::Axis::COLUMN);
        col->setGrow(1.0f);
        col->setMarginRight(15);

        auto* title = new brls::Label();
        title->setFontSize(20);
        title->setText("app/collections/all_catalog"_i18n);
        col->addView(title);

        auto* desc = new brls::Label();
        desc->setFontSize(13);
        desc->setTextColor(nvgRGB(158, 158, 158));
        desc->setText("app/collections/all_catalog_desc"_i18n);
        col->addView(desc);

        row->addView(col);

        auto* count = new brls::Label();
        count->setFontSize(14);
        count->setTextColor(nvgRGB(170, 170, 170));
        count->setText(brls::getStr("app/collections/games_count", std::to_string(g_games.size())));
        row->addView(count);

        row->registerClickAction([](brls::View*) {
            brls::Application::pushActivity(new CatalogView());
            return true;
        });

        listBox->addView(row);
    }

    // Row 1: Favorites
    {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(76);
        row->setWidth(brls::View::AUTO);
        row->setPadding(10, 12, 10, 12);
        row->setMarginBottom(8);
        row->setFocusable(true);

        auto* col = new brls::Box();
        col->setAxis(brls::Axis::COLUMN);
        col->setGrow(1.0f);
        col->setMarginRight(15);

        auto* title = new brls::Label();
        title->setFontSize(20);
        title->setText("app/collections/favorites"_i18n);
        col->addView(title);

        auto* desc = new brls::Label();
        desc->setFontSize(13);
        desc->setTextColor(nvgRGB(158, 158, 158));
        desc->setText("app/collections/favorites_desc"_i18n);
        col->addView(desc);

        row->addView(col);

        auto* count = new brls::Label();
        count->setFontSize(14);
        count->setTextColor(nvgRGB(170, 170, 170));
        count->setText(brls::getStr("app/collections/games_count",
            std::to_string(catalog::FavoritesManager::instance().getFavorites().size())));
        row->addView(count);

        row->registerClickAction([](brls::View*) {
            brls::Application::pushActivity(new FavoritesView());
            return true;
        });

        listBox->addView(row);
    }

    // Collection rows
    const auto& collections = catalog::CollectionsManager::instance().collections();
    for (const auto& info : collections) {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(76);
        row->setWidth(brls::View::AUTO);
        row->setPadding(10, 12, 10, 12);
        row->setMarginBottom(8);
        row->setFocusable(true);

        auto* col = new brls::Box();
        col->setAxis(brls::Axis::COLUMN);
        col->setGrow(1.0f);
        col->setMarginRight(15);

        auto* title = new brls::Label();
        title->setFontSize(20);
        title->setText(info.getName());
        col->addView(title);

        auto* desc = new brls::Label();
        desc->setFontSize(13);
        desc->setTextColor(nvgRGB(158, 158, 158));
        desc->setText(info.getDescription());
        col->addView(desc);

        row->addView(col);

        auto* count = new brls::Label();
        count->setFontSize(14);
        count->setTextColor(nvgRGB(170, 170, 170));
        int cached = cachedCount(info);
        count->setText(cached >= 0 ? brls::getStr("app/collections/games_count", std::to_string(cached)) : "");
        row->addView(count);
        countLabels_.push_back(count);

        row->registerClickAction([info](brls::View*) {
            brls::Application::pushActivity(new CollectionGamesView(info));
            return true;
        });

        listBox->addView(row);
    }

    util::logLine("CollectionsView: built " +
                  std::to_string(collections.size() + 2) + " rows");
}

} // namespace ui
