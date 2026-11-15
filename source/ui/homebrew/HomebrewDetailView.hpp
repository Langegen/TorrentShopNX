#pragma once

// Card of one homebrew package: icon, state, Install / Update / Remove,
// progress, facts, description, changelog, screenshot. Installs run in the
// background; leaving the card does not stop them.

#include <borealis.hpp>

#include <atomic>
#include <memory>
#include <vector>

#include "../../catalog/homebrew_installer.hpp"

namespace ui {

class HomebrewDetailView : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("homebrew_detail_view.xml");

    explicit HomebrewDetailView(homebrew::Package pkg);
    ~HomebrewDetailView() override;
    void onContentAvailable() override;

private:
    void fillTexts();
    void refreshState();
    void onPrimary();
    void onRemove();
    void runInstall(std::vector<homebrew::Package> chain);
    void showProgress(const homebrew::InstallProgress& p, const std::string& prefix);
    void finish();
    // Modules of these packages (this one by default) that can be started /
    // stopped without a reboot.
    std::vector<std::string> controllableModules(const std::vector<std::string>& names = {}) const;
    // After an install: start the modules of everything just installed?
    void offerStartModules(const std::vector<std::string>& names, const std::string& titles);
    void toggleModules();

    BRLS_BIND(brls::Image, icon, "icon");
    BRLS_BIND(brls::Label, statusLabel, "statusLabel");
    BRLS_BIND(brls::Button, primaryButton, "primaryButton");
    BRLS_BIND(brls::Button, secondaryButton, "secondaryButton");
    BRLS_BIND(brls::Button, sysmoduleButton, "sysmoduleButton");
    BRLS_BIND(brls::Box, progressBox, "progressBox");
    BRLS_BIND(brls::Label, progressLabel, "progressLabel");
    BRLS_BIND(brls::Box, progressTrack, "progressTrack");
    BRLS_BIND(brls::Box, progressFill, "progressFill");
    BRLS_BIND(brls::ScrollingFrame, scroll, "scroll");
    BRLS_BIND(brls::Label, titleLabel, "titleLabel");
    BRLS_BIND(brls::Label, authorLabel, "authorLabel");
    BRLS_BIND(brls::Box, chipsBox, "chipsBox");
    BRLS_BIND(brls::Box, factsBox, "factsBox");
    BRLS_BIND(brls::Label, descLabel, "descLabel");
    BRLS_BIND(brls::Label, detailsLabel, "detailsLabel");
    BRLS_BIND(brls::Label, changelogTitle, "changelogTitle");
    BRLS_BIND(brls::Label, changelogLabel, "changelogLabel");
    BRLS_BIND(brls::Image, screenshot, "screenshot");

    homebrew::Package pkg_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<bool> imageToken_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    bool busy_ = false;
};

// User-facing text for an install / removal error.
std::string homebrewErrorText(homebrew::InstallError e);

}  // namespace ui
