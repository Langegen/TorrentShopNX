#include "HomebrewDetailView.hpp"

#include "HomebrewGlue.hpp"
#include "../ThemeManager.hpp"
#include "../style/UiTokens.hpp"
#include "../../catalog/homebrew_view_model.hpp"

#include <algorithm>
#include <cmath>

using namespace brls::literals;

namespace ui {

namespace {

std::array<std::string, 4> sizeUnits() {
    return {"app/homebrew/unit_b"_i18n, "app/homebrew/unit_kb"_i18n, "app/homebrew/unit_mb"_i18n,
            "app/homebrew/unit_gb"_i18n};
}

brls::Label* chip(const std::string& text, NVGcolor bg, NVGcolor fg) {
    auto* l = new brls::Label();
    l->setText("  " + text + "  ");
    l->setFontSize(style::kFontCaption);
    l->setSingleLine(true);
    l->setBackgroundColor(bg);
    l->setTextColor(fg);
    l->setCornerRadius(style::kRadiusChip);
    l->setMarginRight(8.0f);
    l->setHeight(26.0f);
    l->setVerticalAlign(brls::VerticalAlign::CENTER);
    return l;
}

void addFact(brls::Box* box, const std::string& caption, const std::string& value) {
    if (value.empty()) return;
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setMarginBottom(8.0f);
    auto* c = new brls::Label();
    c->setText(caption);
    c->setFontSize(style::kFontSub);
    c->setTextColor(style::textSecondary());
    c->setWidth(190.0f);
    c->setSingleLine(true);
    auto* v = new brls::Label();
    v->setText(value);
    v->setFontSize(style::kFontSub);
    v->setTextColor(style::textPrimary());
    v->setGrow(1.0f);
    row->addView(c);
    row->addView(v);
    box->addView(row);
}

std::string stageText(const homebrew::InstallProgress& p) {
    const int pct = static_cast<int>(std::lround(p.fraction * 100.0f));
    switch (p.stage) {
        case homebrew::InstallProgress::Stage::Prepare: return "app/homebrew/stage_prepare"_i18n;
        case homebrew::InstallProgress::Stage::Download:
            return brls::getStr("app/homebrew/stage_download", std::to_string(pct) + "%");
        case homebrew::InstallProgress::Stage::Verify: return "app/homebrew/stage_verify"_i18n;
        case homebrew::InstallProgress::Stage::Extract:
            return brls::getStr("app/homebrew/stage_extract", std::to_string(pct) + "%");
        case homebrew::InstallProgress::Stage::Finish: return "app/homebrew/stage_finish"_i18n;
        case homebrew::InstallProgress::Stage::Remove: return "app/homebrew/stage_remove"_i18n;
    }
    return "";
}

bool hasSettings(const homebrew::Installed& in) {
    for (const auto& e : in.manifest)
        if (e.op == homebrew::ManifestOp::Get || e.path.rfind("config/", 0) == 0) return true;
    return false;
}

}  // namespace

std::string homebrewErrorText(homebrew::InstallError e) {
    switch (e) {
        case homebrew::InstallError::None: return "";
        case homebrew::InstallError::Cancelled: return "app/homebrew/err_cancelled"_i18n;
        case homebrew::InstallError::Network: return "app/homebrew/err_network"_i18n;
        case homebrew::InstallError::Checksum: return "app/homebrew/err_checksum"_i18n;
        case homebrew::InstallError::NoSpace: return "app/homebrew/err_no_space"_i18n;
        case homebrew::InstallError::Forbidden: return "app/homebrew/err_forbidden"_i18n;
        case homebrew::InstallError::BadArchive: return "app/homebrew/err_bad_archive"_i18n;
        case homebrew::InstallError::Write: return "app/homebrew/err_write"_i18n;
        case homebrew::InstallError::NotInstalled: return "app/homebrew/err_not_installed"_i18n;
    }
    return "";
}

HomebrewDetailView::HomebrewDetailView(homebrew::Package pkg)
    : pkg_(std::move(pkg)), alive_(std::make_shared<std::atomic<bool>>(true)),
      imageToken_(std::make_shared<bool>(true)) {}

HomebrewDetailView::~HomebrewDetailView() {
    alive_->store(false);
    *imageToken_ = false;
}

void HomebrewDetailView::onContentAvailable() {
    progressTrack->setBackgroundColor(style::barTrack());
    progressFill->setBackgroundColor(ThemeManager::instance().getAccentColor());
    factsBox->setBackgroundColor(style::cardBg());

    hb::loadImage(icon, pkg_.icon_url, imageToken_, true);
    if (!pkg_.screen_url.empty()) {
        screenshot->setVisibility(brls::Visibility::VISIBLE);
        hb::loadImage(screenshot, pkg_.screen_url, imageToken_, true);
    }
    fillTexts();
    refreshState();

    primaryButton->registerClickAction([this](brls::View*) {
        onPrimary();
        return true;
    });
    secondaryButton->registerClickAction([this](brls::View*) {
        onRemove();
        return true;
    });

    // The text column scrolls with up / down once it has focus.
    scroll->setFocusable(true);
    scroll->setHideHighlight(true);
    auto scrollBy = [this](float dy) {
        float maxOffset = 0.f;
        if (!scroll->getChildren().empty()) maxOffset = scroll->getChildren().front()->getHeight() - scroll->getHeight();
        const float next = std::clamp(scroll->getContentOffsetY() + dy, 0.f, std::max(0.f, maxOffset));
        scroll->setContentOffsetY(next, true);
    };
    scroll->registerAction("", brls::ControllerButton::BUTTON_DOWN, [scrollBy](brls::View*) {
        scrollBy(120.f);
        return true;
    }, true, true);
    scroll->registerAction("", brls::ControllerButton::BUTTON_UP, [scrollBy](brls::View*) {
        scrollBy(-120.f);
        return true;
    }, true, true);
    scroll->setCustomNavigationRoute(brls::FocusDirection::LEFT, primaryButton);
    primaryButton->setCustomNavigationRoute(brls::FocusDirection::RIGHT, scroll);
    secondaryButton->setCustomNavigationRoute(brls::FocusDirection::RIGHT, scroll);

    // B while installing cancels; otherwise it goes back.
    registerAction("brls/hints/back"_i18n, brls::ControllerButton::BUTTON_B, [this](brls::View*) {
        if (busy_ && cancel_ && !cancel_->load()) {
            cancel_->store(true);
            progressLabel->setText("app/homebrew/cancelling"_i18n);
            return true;
        }
        brls::Application::popActivity();
        return true;
    });
}

void HomebrewDetailView::fillTexts() {
    titleLabel->setText(pkg_.title);
    authorLabel->setText(pkg_.author);

    chipsBox->clearViews();
    const NVGcolor accent = ThemeManager::instance().getAccentColor();
    const NVGcolor soft = style::isLight() ? nvgRGB(232, 236, 242) : nvgRGBA(255, 255, 255, 30);
    if (pkg_.kinds & homebrew::KindApp) chipsBox->addView(chip("app/homebrew/kind_app"_i18n, soft, style::textPrimary()));
    if (pkg_.kinds & homebrew::KindOverlay)
        chipsBox->addView(chip("app/homebrew/kind_overlay"_i18n, soft, style::textPrimary()));
    if (pkg_.kinds & homebrew::KindSysmodule)
        chipsBox->addView(chip("app/homebrew/kind_sysmodule"_i18n, soft, style::textPrimary()));
    if (homebrew::isNew(pkg_, hb::today())) chipsBox->addView(chip("app/homebrew/badge_new"_i18n, accent, hb::textOn(accent)));

    factsBox->clearViews();
    const auto units = sizeUnits();
    addFact(factsBox, "app/homebrew/fact_version"_i18n, pkg_.version);
    addFact(factsBox, "app/homebrew/fact_updated"_i18n, homebrew::displayDate(pkg_.updated));
    addFact(factsBox, "app/homebrew/fact_size"_i18n, homebrew::formatSize(pkg_.download_bytes, units));
    addFact(factsBox, "app/homebrew/fact_unpacked"_i18n, homebrew::formatSize(pkg_.extracted_bytes, units));
    addFact(factsBox, "app/homebrew/fact_source"_i18n,
            pkg_.source == homebrew::Source::Github ? "GitHub" : "Homebrew App Store");
    if (pkg_.downloads > 0) addFact(factsBox, "app/homebrew/fact_downloads"_i18n, std::to_string(pkg_.downloads));
    if (homebrew::isKnownLicense(pkg_.license)) addFact(factsBox, "app/homebrew/fact_license"_i18n, pkg_.license);
    if (!pkg_.depends.empty()) {
        std::string deps;
        for (const auto& d : pkg_.depends) deps += (deps.empty() ? "" : ", ") + d;
        addFact(factsBox, "app/homebrew/fact_requires"_i18n, deps);
    }

    const std::string desc = homebrew::displayDescription(pkg_, brls::Application::getLocale());
    descLabel->setText(desc);
    descLabel->setVisibility(desc.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    detailsLabel->setText(pkg_.details);
    detailsLabel->setVisibility(pkg_.details.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    changelogTitle->setText("app/homebrew/changelog"_i18n);
    std::string log = homebrew::plainText(pkg_.changelog);
    if (log.size() > 3000) log = log.substr(0, 3000) + "…";
    changelogLabel->setText(log);
    const auto vis = log.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE;
    changelogTitle->setVisibility(vis);
    changelogLabel->setVisibility(vis);
}

void HomebrewDetailView::refreshState() {
    auto& cat = homebrew::Catalog::instance();
    const homebrew::Installed in = cat.installed(pkg_.name);
    const homebrew::Status st = homebrew::statusFor(pkg_, in);

    std::string status;
    NVGcolor color = style::textSecondary();
    std::string primary;
    bool canRemove = false;
    if (st == homebrew::Status::NotInstalled) {
        status = "app/homebrew/status_not_installed"_i18n;
        primary = "app/homebrew/btn_install"_i18n;
    } else if (in.untracked) {
        status = "app/homebrew/status_untracked"_i18n;
        color = style::stateOk();
        primary = "app/homebrew/btn_reinstall"_i18n;
    } else if (st == homebrew::Status::UpdateAvailable) {
        status = in.version.empty() ? brls::getStr("app/homebrew/status_update_unknown", pkg_.version)
                                    : brls::getStr("app/homebrew/status_update", in.version, pkg_.version);
        color = style::stateWarn();
        primary = "app/homebrew/btn_update"_i18n;
        canRemove = true;
    } else {
        status = brls::getStr("app/homebrew/status_installed", in.version);
        color = style::stateOk();
        primary = "app/homebrew/btn_reinstall"_i18n;
        canRemove = true;
    }
    statusLabel->setText(status);
    statusLabel->setTextColor(color);
    primaryButton->setText(primary);
    secondaryButton->setText("app/homebrew/btn_remove"_i18n);
    secondaryButton->setVisibility(canRemove ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
}

void HomebrewDetailView::onPrimary() {
    if (busy_) return;
    hb::haptic(2);
    std::vector<homebrew::Package> deps = homebrew::missingDependencies(pkg_, homebrew::Catalog::instance());
    if (deps.empty()) {
        runInstall({pkg_});
        return;
    }
    std::string names;
    for (const auto& d : deps) names += (names.empty() ? "" : ", ") + d.title;
    auto* dialog = new brls::Dialog(brls::getStr("app/homebrew/deps_title", pkg_.title, names));
    auto alive = alive_;
    dialog->addButton("app/homebrew/deps_install_all"_i18n, [this, alive, deps]() {
        if (!alive->load()) return;
        std::vector<homebrew::Package> chain = deps;
        chain.push_back(pkg_);
        runInstall(chain);
    });
    dialog->addButton("app/homebrew/deps_only_this"_i18n, [this, alive]() {
        if (alive->load()) runInstall({pkg_});
    });
    dialog->setCancelable(true);
    dialog->open();
}

void HomebrewDetailView::runInstall(std::vector<homebrew::Package> chain) {
    busy_ = true;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    progressBox->setVisibility(brls::Visibility::VISIBLE);
    primaryButton->setState(brls::ButtonState::DISABLED);
    secondaryButton->setVisibility(brls::Visibility::GONE);
    progressLabel->setText("app/homebrew/stage_prepare"_i18n);
    progressFill->setWidth(0.f);

    auto alive = alive_;
    auto cancel = cancel_;
    const std::string mainTitle = pkg_.title;
    const bool wasInstalled = homebrew::Catalog::instance().installed(pkg_.name).present;
    brls::async([this, alive, cancel, chain, mainTitle, wasInstalled]() {
        homebrew::InstallResult last;
        bool sysmodule = false;
        for (size_t i = 0; i < chain.size(); ++i) {
            const homebrew::Package& p = chain[i];
            const std::string prefix = chain.size() > 1 ? brls::getStr("app/homebrew/installing_n", p.title,
                                                                       std::to_string(i + 1), std::to_string(chain.size())) + "  "
                                                        : "";
            // Unpacking reports every 64 KB: pass on only a new stage or a new percent.
            auto lastSent = std::make_shared<std::pair<int, int>>(-1, -1);
            last = homebrew::Installer::instance().install(
                p, homebrew::Catalog::instance().installed(p.name),
                [this, alive, prefix, lastSent](const homebrew::InstallProgress& pr) {
                    const std::pair<int, int> now{static_cast<int>(pr.stage), static_cast<int>(pr.fraction * 100.f)};
                    if (now == *lastSent) return;
                    *lastSent = now;
                    brls::sync([this, alive, prefix, pr]() {
                        if (alive->load()) showProgress(pr, prefix);
                    });
                },
                cancel);
            if (!last.ok) break;
            if (p.kinds & homebrew::KindSysmodule) sysmodule = true;
        }
        homebrew::Catalog::instance().rescanInstalled();
        brls::sync([this, alive, last, mainTitle, wasInstalled, sysmodule]() {
            hb::packagesChanged();
            if (last.ok) {
                brls::Application::notify(brls::getStr(wasInstalled ? "app/homebrew/done_update" : "app/homebrew/done_install", mainTitle));
                if (sysmodule) brls::Application::notify(brls::getStr("app/homebrew/sysmodule_reboot", mainTitle));
            } else {
                brls::Application::notify(homebrewErrorText(last.error));
            }
            if (alive->load()) finish();
        });
    });
}

void HomebrewDetailView::showProgress(const homebrew::InstallProgress& p, const std::string& prefix) {
    if (cancel_ && cancel_->load()) return;
    progressLabel->setText(prefix + stageText(p));
    progressFill->setWidth(256.f * std::clamp(p.fraction, 0.f, 1.f));
}

void HomebrewDetailView::finish() {
    busy_ = false;
    progressBox->setVisibility(brls::Visibility::GONE);
    primaryButton->setState(brls::ButtonState::ENABLED);
    refreshState();
    brls::Application::giveFocus(primaryButton);
}

void HomebrewDetailView::onRemove() {
    if (busy_) return;
    const homebrew::Installed in = homebrew::Catalog::instance().installed(pkg_.name);
    auto* dialog = new brls::Dialog(brls::getStr("app/homebrew/remove_title", pkg_.title));
    auto alive = alive_;
    auto run = [this, alive](bool keepSettings) {
        if (!alive->load()) return;
        busy_ = true;
        progressBox->setVisibility(brls::Visibility::VISIBLE);
        progressLabel->setText("app/homebrew/stage_remove"_i18n);
        progressFill->setWidth(0.f);
        const std::string name = pkg_.name, title = pkg_.title;
        brls::async([this, alive, name, title, keepSettings]() {
            const homebrew::InstallResult r = homebrew::Installer::instance().uninstall(name, keepSettings, nullptr);
            homebrew::Catalog::instance().rescanInstalled();
            brls::sync([this, alive, r, title]() {
                hb::packagesChanged();
                brls::Application::notify(r.ok ? brls::getStr("app/homebrew/done_remove", title) : homebrewErrorText(r.error));
                if (alive->load()) finish();
            });
        });
    };
    if (hasSettings(in)) {
        dialog->addButton("app/homebrew/remove_keep"_i18n, [run]() { run(true); });
        dialog->addButton("app/homebrew/remove_all"_i18n, [run]() { run(false); });
    } else {
        dialog->addButton("app/homebrew/btn_remove"_i18n, [run]() { run(false); });
    }
    dialog->setCancelable(true);
    dialog->open();
}

}  // namespace ui
