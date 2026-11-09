#include "HomebrewView.hpp"

#include "HomebrewDetailView.hpp"
#include "HomebrewGlue.hpp"
#include "SysmodulesView.hpp"
#include "../EmptyState.hpp"
#include "../ThemeManager.hpp"
#include "../style/HeaderHints.hpp"
#include "../style/UiTokens.hpp"

#include <algorithm>

using namespace brls::literals;

namespace ui {

namespace {

constexpr float kCardWidth = 184.0f;
constexpr float kCardHeight = 236.0f;
constexpr float kIconSize = 156.0f;
constexpr float kRowHeight = 252.0f;

const char* categoryKey(homebrew::Category c) {
    switch (c) {
        case homebrew::Category::All: return "app/homebrew/cat_all";
        case homebrew::Category::Apps: return "app/homebrew/cat_apps";
        case homebrew::Category::Games: return "app/homebrew/cat_games";
        case homebrew::Category::Emulators: return "app/homebrew/cat_emus";
        case homebrew::Category::Overlays: return "app/homebrew/cat_overlays";
        case homebrew::Category::Sysmodules: return "app/homebrew/cat_sysmodules";
        case homebrew::Category::Themes: return "app/homebrew/cat_themes";
        case homebrew::Category::Installed: return "app/homebrew/cat_installed";
        case homebrew::Category::Count: break;
    }
    return "";
}

const char* sortKey(homebrew::SortMode s) {
    switch (s) {
        case homebrew::SortMode::Popular: return "app/homebrew/sort_popular";
        case homebrew::SortMode::Updated: return "app/homebrew/sort_updated";
        case homebrew::SortMode::Name: return "app/homebrew/sort_name";
        case homebrew::SortMode::Count: break;
    }
    return "";
}

// Small colored label over the icon: Installed / Update / New.
void setBadge(brls::Label* badge, const homebrew::ViewItem& it) {
    std::string text;
    NVGcolor bg = nvgRGB(0, 0, 0);
    if (it.status == homebrew::Status::UpdateAvailable) {
        text = "app/homebrew/badge_update"_i18n;
        bg = nvgRGB(214, 120, 0);
    } else if (it.status == homebrew::Status::Installed) {
        text = "app/homebrew/badge_installed"_i18n;
        bg = nvgRGB(30, 140, 80);
    } else if (it.is_new) {
        text = "app/homebrew/badge_new"_i18n;
        bg = ThemeManager::instance().getAccentColor();
    }
    if (text.empty()) {
        badge->setVisibility(brls::Visibility::GONE);
        return;
    }
    badge->setText(" " + text + " ");
    badge->setBackgroundColor(bg);
    badge->setTextColor(hb::textOn(bg));
    badge->setVisibility(brls::Visibility::VISIBLE);
}

}  // namespace

// ---------------------------------------------------------------------------
// Row of cards

int HomebrewRowCell::s_lastFocusedColumn = 0;

HomebrewRowCell::HomebrewRowCell() : imageToken(std::make_shared<bool>(true)) {
    setAxis(brls::Axis::ROW);
    setJustifyContent(brls::JustifyContent::SPACE_EVENLY);
    setAlignItems(brls::AlignItems::FLEX_START);
    setHeight(kRowHeight);
    setPaddingTop(6.0f);

    for (int i = 0; i < kColumns; ++i) {
        Card& c = cards[i];
        c.box = new brls::Box(brls::Axis::COLUMN);
        c.box->setWidth(kCardWidth);
        c.box->setHeight(kCardHeight);
        c.box->setPadding(6.0f, 6.0f, 6.0f, 6.0f);
        c.box->setCornerRadius(style::kRadiusCard);
        c.box->setAlignItems(brls::AlignItems::CENTER);
        c.box->setFocusable(true);
        c.box->setHideHighlightBackground(false);

        auto* iconBox = new brls::Box();
        iconBox->setWidth(kIconSize);
        iconBox->setHeight(kIconSize);
        iconBox->setCornerRadius(style::kRadiusCard);
        iconBox->setBackgroundColor(brls::Application::getTheme().getColor("brls/sidebar/separator"));

        c.icon = new brls::Image();
        c.icon->setWidth(kIconSize);
        c.icon->setHeight(kIconSize);
        c.icon->setCornerRadius(style::kRadiusCard);
        c.icon->setScalingType(brls::ImageScalingType::FILL);
        iconBox->addView(c.icon);

        c.badge = new brls::Label();
        c.badge->setFontSize(13.0f);
        c.badge->setTextColor(nvgRGB(255, 255, 255));
        c.badge->setCornerRadius(4.0f);
        c.badge->setSingleLine(true);
        c.badge->setPositionType(brls::PositionType::ABSOLUTE);
        c.badge->setPositionTop(6.0f);
        c.badge->setPositionRight(6.0f);
        c.badge->setVisibility(brls::Visibility::GONE);
        iconBox->addView(c.badge);
        c.box->addView(iconBox);

        c.title = new brls::Label();
        c.title->setWidth(kCardWidth - 12.0f);
        c.title->setHeight(22.0f);
        c.title->setMarginTop(8.0f);
        c.title->setFontSize(style::kFontSub);
        c.title->setSingleLine(true);
        c.title->setAutoAnimate(false);
        c.title->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        c.box->addView(c.title);

        c.sub = new brls::Label();
        c.sub->setWidth(kCardWidth - 12.0f);
        c.sub->setFontSize(style::kFontCaption);
        c.sub->setTextColor(style::textSecondary());
        c.sub->setSingleLine(true);
        c.sub->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        c.box->addView(c.sub);

        addView(c.box);
    }
}

HomebrewRowCell* HomebrewRowCell::create() { return new HomebrewRowCell(); }

void HomebrewRowCell::prepareForReuse() {
    brls::RecyclerCell::prepareForReuse();
    *imageToken = false;  // images still on their way belong to the old row
    imageToken = std::make_shared<bool>(true);
    for (auto& c : cards) {
        if (c.box->isFocused()) c.box->onFocusLost();
        c.box->setHighlighted(false);
        c.box->setHighlightProgress(0.0f);
        c.title->setAnimated(false);
        c.icon->clear();
        c.badge->setVisibility(brls::Visibility::GONE);
    }
}

void HomebrewRowCell::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
                           brls::FrameContext* ctx) {
    setLineTop(0.0f);
    setLineBottom(0.0f);
    brls::RecyclerCell::draw(vg, x, y, width, height, style, ctx);
}

brls::View* HomebrewRowCell::getDefaultFocus() {
    for (int i = std::min(s_lastFocusedColumn, kColumns - 1); i >= 0; --i)
        if (cards[i].box->getVisibility() == brls::Visibility::VISIBLE) return cards[i].box;
    return cards[0].box;
}

// ---------------------------------------------------------------------------
// Data source

class HomebrewView::DataSource : public brls::RecyclerDataSource {
public:
    explicit DataSource(HomebrewView* v) : view_(v) {}
    int numberOfSections(brls::RecyclerFrame*) override { return 1; }
    int numberOfRows(brls::RecyclerFrame*, int) override {
        return static_cast<int>((view_->items_.size() + HomebrewRowCell::kColumns - 1) / HomebrewRowCell::kColumns);
    }
    float heightForRow(brls::RecyclerFrame*, brls::IndexPath) override { return kRowHeight; }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        auto* cell = static_cast<HomebrewRowCell*>(recycler->dequeueReusableCell("Row"));
        const int row = index.row;
        auto packages = view_->packages_;  // keeps the list the items point into alive
        for (int i = 0; i < HomebrewRowCell::kColumns; ++i) {
            auto& c = cell->cards[i];
            const size_t idx = static_cast<size_t>(row) * HomebrewRowCell::kColumns + i;
            c.box->getFocusEvent()->clear();
            c.box->getFocusLostEvent()->clear();
            if (idx >= view_->items_.size()) {
                c.box->setVisibility(brls::Visibility::INVISIBLE);
                c.box->setFocusable(false);
                c.box->registerClickAction([](brls::View*) { return false; });
                continue;
            }
            const homebrew::ViewItem it = view_->items_[idx];
            const homebrew::Package& p = *it.pkg;
            c.box->setVisibility(brls::Visibility::VISIBLE);
            c.box->setFocusable(true);
            c.title->setText(p.title);
            std::string sub = p.author;
            if (!p.version.empty()) sub += (sub.empty() ? "" : " · ") + p.version;
            c.sub->setText(sub);
            setBadge(c.badge, it);
            hb::loadImage(c.icon, p.icon_url, cell->imageToken, false, row, i);

            brls::Label* title = c.title;
            const std::string name = p.name;
            HomebrewView* view = view_;
            c.box->getFocusEvent()->subscribe([title, i, view, name](brls::View*) {
                HomebrewRowCell::s_lastFocusedColumn = i;
                view->focusedName_ = name;
                title->setAnimated(true);
            });
            c.box->getFocusLostEvent()->subscribe([title](brls::View*) { title->setAnimated(false); });
            c.box->registerClickAction([name, packages](brls::View*) {
                hb::haptic(2);
                for (const auto& pkg : *packages)
                    if (pkg.name == name) {
                        brls::Application::pushActivity(new HomebrewDetailView(pkg));
                        break;
                    }
                return true;
            });
        }
        return cell;
    }

private:
    HomebrewView* view_;
};

// ---------------------------------------------------------------------------

HomebrewView::HomebrewView() : alive_(std::make_shared<std::atomic<bool>>(true)) {}

HomebrewView::~HomebrewView() { alive_->store(false); }

void HomebrewView::onContentAvailable() {
    hb::rememberSection("homebrew");
    titleLabel->setText("app/homebrew/title"_i18n);
    otherTabLabel->setText(style::buttonHint(brls::ControllerButton::BUTTON_RB, "app/homebrew/tab_retro"_i18n));
    otherTabLabel->addGestureRecognizer(new brls::TapGestureRecognizer(otherTabLabel, [] { hb::showRetroSection(); }));

    emptyState_ = new EmptyState();
    emptyHolder->addView(emptyState_);

    recycler->registerCell("Row", []() { return HomebrewRowCell::create(); });
    recycler->estimatedRowHeight = kRowHeight;
    recycler->setDataSource(new DataSource(this));

    buildChips();

    // Section tabs.
    auto toRetro = [](brls::View*) {
        hb::showRetroSection();
        return true;
    };
    registerAction("", brls::ControllerButton::BUTTON_LB, toRetro, true);
    registerAction("", brls::ControllerButton::BUTTON_RB, toRetro, true);

    // Categories.
    registerAction("", brls::ControllerButton::BUTTON_RT, [this](brls::View*) {
        setCategory(static_cast<homebrew::Category>((static_cast<size_t>(category_) + 1) % homebrew::kCategoryCount));
        return true;
    }, true);
    registerAction("", brls::ControllerButton::BUTTON_LT, [this](brls::View*) {
        setCategory(static_cast<homebrew::Category>((static_cast<size_t>(category_) + homebrew::kCategoryCount - 1) %
                                                    homebrew::kCategoryCount));
        return true;
    }, true);

    registerAction("app/homebrew/hint_search"_i18n, brls::ControllerButton::BUTTON_X, [this](brls::View*) {
        auto alive = alive_;
        brls::Application::getImeManager()->openForText(
            [this, alive](std::string text) {
                if (!alive->load()) return;
                query_ = text;
                rebuild(false);
            },
            "app/homebrew/hint_search"_i18n, "app/homebrew/search_hint"_i18n, 40, query_);
        return true;
    });
    registerAction("app/homebrew/hint_sort"_i18n, brls::ControllerButton::BUTTON_Y, [this](brls::View*) {
        sort_ = static_cast<homebrew::SortMode>((static_cast<size_t>(sort_) + 1) % static_cast<size_t>(homebrew::SortMode::Count));
        brls::Application::notify(brls::getStr("app/homebrew/sort_notify", brls::getStr(sortKey(sort_))));
        rebuild(false);
        return true;
    });
    // Hidden from the footer: it fits five hints, and "+" (update all) needs the room.
    // The catalog refreshes on its own every 6 hours.
    registerAction("app/homebrew/hint_refresh"_i18n, brls::ControllerButton::BUTTON_BACK, [this](brls::View*) {
        brls::Application::notify("app/homebrew/refreshing"_i18n);
        loadCatalog(true);
        return true;
    }, true);

    loadCatalog(false);
}

void HomebrewView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    if (homebrew::Catalog::instance().generation() != seenGeneration_) rebuild(true);
}

void HomebrewView::onResume() {
    brls::Activity::onResume();
    // Back from a package card: something may have been installed or removed.
    // Next frame: popActivity gives the focus back to the old card view right
    // after this, and a rebuild may have put another package into that view.
    if (homebrew::Catalog::instance().generation() == seenGeneration_) return;
    auto alive = alive_;
    brls::sync([this, alive]() {
        if (alive->load()) rebuild(true);
    });
}

void HomebrewView::loadCatalog(bool force) {
    auto& cat = homebrew::Catalog::instance();
    if (cat.loaded()) rebuild(false);
    if (loading_) return;
    loading_ = true;
    loadFailed_ = false;
    if (!cat.loaded()) showEmpty();
    auto alive = alive_;
    brls::async([this, alive, force]() {
        auto& c = homebrew::Catalog::instance();
        if (!c.loaded()) c.loadCached();
        if (alive->load()) brls::sync([this, alive]() {
            if (alive->load() && homebrew::Catalog::instance().loaded()) rebuild(false);
        });
        const homebrew::RefreshResult r = c.refresh(force);
        brls::sync([this, alive, r, force]() {
            if (!alive->load()) return;
            loading_ = false;
            loadFailed_ = !homebrew::Catalog::instance().loaded();
            if (force) brls::Application::notify(r.ok ? "app/homebrew/refreshed"_i18n : "app/homebrew/refresh_failed"_i18n);
            rebuild(true);
        });
    });
}

void HomebrewView::rebuild(bool keepFocus) {
    auto& cat = homebrew::Catalog::instance();
    seenGeneration_ = cat.generation();
    packages_ = cat.snapshot();
    installed_ = cat.installedSnapshot();
    items_ = homebrew::buildView(*packages_, *installed_, category_, sort_, query_, hb::today());
    counts_ = homebrew::countByCategory(*packages_, *installed_);
    updateChips();
    updateHeader();
    updateStartAction();

    if (items_.empty()) {
        showEmpty();
        return;
    }
    emptyHolder->setVisibility(brls::Visibility::GONE);
    recycler->setVisibility(brls::Visibility::VISIBLE);

    // Where the focus goes: the same package if it is still listed, else the first one.
    int target = 0;
    if (keepFocus && !focusedName_.empty()) {
        for (size_t i = 0; i < items_.size(); ++i)
            if (items_[i].pkg->name == focusedName_) target = static_cast<int>(i);
    }
    HomebrewRowCell::s_lastFocusedColumn = target % HomebrewRowCell::kColumns;
    recycler->reloadData();
    recycler->selectRowAt(brls::IndexPath(0, target / HomebrewRowCell::kColumns), false);
    // Only while this screen is on top: a background refresh must not take the
    // focus from an open package card.
    const auto& stack = brls::Application::getActivitiesStack();
    if (!stack.empty() && stack.back() == this) brls::Application::giveFocus(recycler);
}

void HomebrewView::showEmpty() {
    recycler->setVisibility(brls::Visibility::GONE);
    emptyHolder->setVisibility(brls::Visibility::VISIBLE);
    auto& cat = homebrew::Catalog::instance();
    if (!cat.loaded()) {
        if (loadFailed_) {
            emptyState_->setContent(DashboardIcon::Kind::CloudError, "app/homebrew/error_title"_i18n,
                                    "app/homebrew/error_sub"_i18n, "app/homebrew/retry"_i18n, [this] { loadCatalog(true); });
        } else {
            emptyState_->setContent(DashboardIcon::Kind::Download, "app/homebrew/loading_title"_i18n,
                                    "app/homebrew/loading_sub"_i18n);
        }
    } else if (!query_.empty()) {
        emptyState_->setContent(DashboardIcon::Kind::Search, "app/homebrew/empty_title"_i18n, "app/homebrew/empty_sub"_i18n,
                                "app/homebrew/reset_search"_i18n, [this] {
                                    query_.clear();
                                    rebuild(false);
                                });
    } else if (category_ == homebrew::Category::Installed) {
        emptyState_->setContent(DashboardIcon::Kind::GameCards, "app/homebrew/installed_empty_title"_i18n,
                                "app/homebrew/installed_empty_sub"_i18n);
    } else {
        emptyState_->setContent(DashboardIcon::Kind::Search, "app/homebrew/empty_title"_i18n,
                                "app/homebrew/empty_cat_sub"_i18n);
    }
    const auto& stack = brls::Application::getActivitiesStack();
    if (!stack.empty() && stack.back() == this) brls::Application::giveFocus(emptyState_->actionView());
}

void HomebrewView::buildChips() {
    chipsBox->clearViews();
    chips_.clear();
    for (size_t i = 0; i < homebrew::kCategoryCount; ++i) {
        auto* chip = new brls::Label();
        chip->setFontSize(style::kFontSub);
        chip->setSingleLine(true);
        chip->setCornerRadius(style::kRadiusChip + 8.0f);
        chip->setMarginRight(8.0f);
        chip->setVerticalAlign(brls::VerticalAlign::CENTER);
        chip->setHeight(30.0f);
        const auto cat = static_cast<homebrew::Category>(i);
        chip->addGestureRecognizer(new brls::TapGestureRecognizer(chip, [this, cat] { setCategory(cat); }));
        chipsBox->addView(chip);
        chips_.push_back(chip);
    }
    updateChips();
}

void HomebrewView::updateChips() {
    const NVGcolor accent = ThemeManager::instance().getAccentColor();
    for (size_t i = 0; i < chips_.size(); ++i) {
        const auto cat = static_cast<homebrew::Category>(i);
        std::string text = brls::getStr(categoryKey(cat));
        if (counts_[i] > 0) text += "  " + std::to_string(counts_[i]);
        if (cat == homebrew::Category::Installed) {
            const int updates = homebrew::Catalog::instance().updatesAvailable();
            if (updates > 0) text += "  ↑" + std::to_string(updates);
        }
        chips_[i]->setText("  " + text + "  ");
        const bool active = cat == category_;
        chips_[i]->setBackgroundColor(active ? accent : nvgRGBA(0, 0, 0, 0));
        chips_[i]->setTextColor(active ? hb::textOn(accent) : style::textSecondary());
    }
}

void HomebrewView::updateHeader() {
    const auto& cat = homebrew::Catalog::instance();
    std::string stats = brls::getStr("app/homebrew/stats", std::to_string(packages_ ? packages_->size() : 0));
    const int updates = cat.updatesAvailable();
    if (updates > 0) stats += "  ·  " + brls::getStr("app/homebrew/stats_updates", std::to_string(updates));
    if (!query_.empty()) stats += "  ·  «" + query_ + "»";
    if (!updateAllStatus_.empty()) stats = updateAllStatus_;
    statsHint->setText(style::headerMeta(
        stats, {style::buttonHint(brls::ControllerButton::BUTTON_LT, brls::ControllerButton::BUTTON_RT,
                                  "app/homebrew/hint_category"_i18n),
                brls::getStr(sortKey(sort_))}));
}

void HomebrewView::updateStartAction() {
    if (category_ == homebrew::Category::Sysmodules) {
        registerAction("app/homebrew/sysmodules_manage"_i18n, brls::ControllerButton::BUTTON_START, [](brls::View*) {
            brls::Application::pushActivity(new SysmodulesView());
            return true;
        });
    } else {
        const int updates = homebrew::Catalog::instance().updatesAvailable();
        if (updates > 0 && !updatingAll_) {
            registerAction(brls::getStr("app/homebrew/update_all", std::to_string(updates)),
                           brls::ControllerButton::BUTTON_START, [this](brls::View*) {
                               updateAll();
                               return true;
                           });
        } else {
            registerAction("", brls::ControllerButton::BUTTON_START, [](brls::View*) { return true; }, true);
        }
    }
    brls::Application::getGlobalHintsUpdateEvent()->fire();
}

void HomebrewView::updateAll() {
    if (updatingAll_) return;
    // Everything the registry knows with a newer version in the store.
    std::vector<homebrew::Package> todo;
    const auto& cat = homebrew::Catalog::instance();
    for (const auto& p : *packages_) {
        const homebrew::Installed in = cat.installed(p.name);
        if (in.present && !in.untracked && homebrew::statusFor(p, in) == homebrew::Status::UpdateAvailable)
            todo.push_back(p);
    }
    if (todo.empty()) return;

    auto* dialog = new brls::Dialog(brls::getStr("app/homebrew/update_all_confirm", std::to_string(todo.size())));
    auto alive = alive_;
    dialog->addButton("app/homebrew/btn_update"_i18n, [this, alive, todo]() {
        if (!alive->load()) return;
        updatingAll_ = true;
        updateStartAction();
        brls::async([this, alive, todo]() {
            int ok = 0;
            for (size_t i = 0; i < todo.size(); ++i) {
                const auto& p = todo[i];
                const std::string status = brls::getStr("app/homebrew/update_all_progress", std::to_string(i + 1),
                                                         std::to_string(todo.size()), p.title);
                brls::sync([this, alive, status]() {
                    if (!alive->load()) return;
                    updateAllStatus_ = status;
                    updateHeader();
                });
                const auto r = homebrew::Installer::instance().install(
                    p, homebrew::Catalog::instance().installed(p.name), nullptr, nullptr);
                if (r.ok) ++ok;
            }
            homebrew::Catalog::instance().rescanInstalled();
            const int total = static_cast<int>(todo.size());
            brls::sync([this, alive, ok, total]() {
                hb::packagesChanged();
                brls::Application::notify(brls::getStr("app/homebrew/update_all_done", std::to_string(ok),
                                                       std::to_string(total)));
                if (!alive->load()) return;
                updatingAll_ = false;
                updateAllStatus_.clear();
                rebuild(true);
            });
        });
    });
    dialog->setCancelable(true);
    dialog->open();
}

void HomebrewView::setCategory(homebrew::Category c) {
    if (c == category_) return;
    category_ = c;
    hb::haptic(1);
    rebuild(false);
}

}  // namespace ui
