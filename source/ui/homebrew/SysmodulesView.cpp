#include "SysmodulesView.hpp"

#include "HomebrewGlue.hpp"
#include "../EmptyState.hpp"
#include "../style/UiTokens.hpp"
#include "../../catalog/homebrew_catalog.hpp"

using namespace brls::literals;

namespace ui {

void SysmodulesView::onContentAvailable() {
    titleLabel->setText("app/homebrew/sysmodules_title"_i18n);
    noteLabel->setText(hb::canControlSysmodules() ? "app/homebrew/sysmodules_note"_i18n
                                                  : "app/homebrew/sysmodules_note_reboot"_i18n);
    emptyState_ = new EmptyState();
    emptyHolder->addView(emptyState_);
    rebuild("");
}

void SysmodulesView::rebuildLater(const std::string& focusTid) {
    auto alive = alive_;
    brls::sync([this, alive, focusTid]() {
        if (alive->load()) rebuild(focusTid);
    });
}

void SysmodulesView::rebuild(const std::string& focusTid) {
    const auto pkgs = homebrew::Catalog::instance().snapshot();
    const auto installed = homebrew::Catalog::instance().installedSnapshot();
    modules_ = homebrew::scanSysmodules(hb::sdRoot(), pkgs.get(), installed.get());

    int running = 0;
    for (const auto& m : modules_)
        if (hb::sysmoduleRunning(m.tid)) ++running;
    statsHint->setText(brls::getStr("app/homebrew/sysmodules_stats", std::to_string(modules_.size()),
                                    std::to_string(running)));

    listBox->clearViews();
    if (modules_.empty()) {
        scroll->setVisibility(brls::Visibility::GONE);
        emptyHolder->setVisibility(brls::Visibility::VISIBLE);
        emptyState_->setContent(DashboardIcon::Kind::Chip, "app/homebrew/sysmodules_empty_title"_i18n,
                                "app/homebrew/sysmodules_empty_sub"_i18n);
        brls::Application::giveFocus(emptyState_->actionView());
        return;
    }
    emptyHolder->setVisibility(brls::Visibility::GONE);
    scroll->setVisibility(brls::Visibility::VISIBLE);

    brls::View* focus = nullptr;
    for (const auto& m : modules_) {
        brls::Box* row = makeRow(m);
        listBox->addView(row);
        if (!focus || m.tid == focusTid) focus = row;
    }
    if (focus) brls::Application::giveFocus(focus);
}

brls::Box* SysmodulesView::makeRow(const homebrew::SysmoduleInfo& m) {
    const bool running = hb::sysmoduleRunning(m.tid);

    auto* row = new brls::Box(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setHeight(72.0f);
    row->setPadding(8.0f, 16.0f, 8.0f, 16.0f);
    row->setMarginBottom(8.0f);
    row->setCornerRadius(style::kRadiusCard);
    row->setBackgroundColor(style::cardBg());
    row->setFocusable(true);

    auto* left = new brls::Box(brls::Axis::COLUMN);
    left->setGrow(1.0f);
    auto* name = new brls::Label();
    name->setText(m.name);
    name->setFontSize(style::kFontItemTitle);
    name->setSingleLine(true);
    left->addView(name);
    auto* sub = new brls::Label();
    sub->setText(m.package.empty() ? m.tid : m.tid + "  ·  " + "app/homebrew/sysmodules_from_store"_i18n);
    sub->setFontSize(style::kFontCaption);
    sub->setTextColor(style::textSecondary());
    left->addView(sub);
    row->addView(left);

    auto* state = new brls::Label();
    state->setFontSize(style::kFontSub);
    state->setMarginRight(24.0f);
    if (running) {
        state->setText("app/homebrew/sysmodule_running"_i18n);
        state->setTextColor(style::stateOk());
    } else {
        state->setText("app/homebrew/sysmodule_stopped"_i18n);
        state->setTextColor(style::textSecondary());
    }
    row->addView(state);

    auto* autostart = new brls::Label();
    autostart->setFontSize(style::kFontSub);
    autostart->setWidth(200.0f);
    autostart->setText(m.autostart ? "app/homebrew/autostart_on"_i18n : "app/homebrew/autostart_off"_i18n);
    autostart->setTextColor(m.autostart ? style::textPrimary() : style::textSecondary());
    row->addView(autostart);

    const std::string tid = m.tid, title = m.name;
    const bool requiresReboot = m.requires_reboot;
    if (hb::canControlSysmodules() && !requiresReboot) {
        row->registerAction(running ? "app/homebrew/sysmodule_stop"_i18n : "app/homebrew/sysmodule_start"_i18n,
                            brls::ControllerButton::BUTTON_A, [this, tid, title, running](brls::View*) {
                                std::string err;
                                const bool ok = running ? hb::stopSysmodule(tid, err) : hb::startSysmodule(tid, err);
                                brls::Application::notify(
                                    ok ? brls::getStr(running ? "app/homebrew/sysmodule_stopped_fmt"
                                                              : "app/homebrew/sysmodule_started_fmt", title)
                                       : brls::getStr("app/homebrew/sysmodule_failed_fmt", title));
                                rebuildLater(tid);
                                return true;
                            });
    } else if (requiresReboot) {
        row->registerAction("", brls::ControllerButton::BUTTON_A, [](brls::View*) {
            brls::Application::notify("app/homebrew/sysmodule_needs_reboot"_i18n);
            return true;
        }, true);
    }
    const bool on = m.autostart;
    row->registerAction(on ? "app/homebrew/autostart_disable"_i18n : "app/homebrew/autostart_enable"_i18n,
                        brls::ControllerButton::BUTTON_Y, [this, tid, on](brls::View*) {
                            std::string err;
                            if (!homebrew::setSysmoduleAutostart(hb::sdRoot(), tid, !on, err))
                                brls::Application::notify("app/homebrew/err_write"_i18n);
                            rebuildLater(tid);
                            return true;
                        });
    return row;
}

}  // namespace ui
