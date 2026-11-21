#include "CompanionInstallDialog.hpp"

#include "../GameData.hpp"
#include "../catalog/companion_installer.hpp"
#include "../catalog/game_version.hpp"
#include "../catalog/release_finder.h"
#include "../utils/app_paths.h"
#include "../utils/log.h"
#include "ThemeManager.hpp"
#include "style/UiTokens.hpp"

#include <borealis.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>

using namespace brls::literals;

namespace ui {

namespace {

std::string formatSizeMb(int64_t bytes) {
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << mb << " MB";
    return ss.str();
}

} // namespace

void showCompanionInstallDialog(const std::string& repo,
                                const std::string& appName,
                                const std::string& defaultFileName,
                                const std::string& destRelativePath,
                                std::function<void(bool success)> onComplete) {
    bool isLight = ThemeManager::instance().isCurrentThemeLight();

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(480.0f);
    content->setPadding(20.0f);
    content->setAlignItems(brls::AlignItems::STRETCH);

    auto* titleLabel = new brls::Label();
    titleLabel->setText("app/companion/installing_title"_i18n + appName);
    titleLabel->setFontSize(20.0f);
    titleLabel->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    titleLabel->setMarginBottom(8.0f);
    content->addView(titleLabel);

    auto* statusLabel = new brls::Label();
    statusLabel->setText("app/companion/preparing"_i18n);
    statusLabel->setFontSize(14.0f);
    statusLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    statusLabel->setMarginBottom(12.0f);
    content->addView(statusLabel);

    auto* progressBg = new brls::Box();
    progressBg->setAxis(brls::Axis::ROW);
    progressBg->setWidth(440.0f);
    progressBg->setHeight(10.0f);
    progressBg->setCornerRadius(5.0f);
    progressBg->setBackgroundColor(isLight ? nvgRGBA(0, 0, 0, 25) : nvgRGBA(42, 45, 52, 255));
    progressBg->setMarginBottom(8.0f);

    auto* progressFill = new brls::Box();
    progressFill->setWidth(0.0f);
    progressFill->setHeight(10.0f);
    progressFill->setCornerRadius(5.0f);
    progressFill->setBackgroundColor(style::stateOk());
    progressBg->addView(progressFill);
    content->addView(progressBg);

    auto* statsLabel = new brls::Label();
    statsLabel->setText("0.0 MB");
    statsLabel->setFontSize(14.0f);
    statsLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    statsLabel->setMarginBottom(14.0f);
    content->addView(statsLabel);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(false);

    auto* applet = dialog->getAppletFrame();
    if (applet) {
        applet->setWidth(520.0f);
        applet->setCornerRadius(14.0f);
        applet->setBackgroundColor(isLight ? nvgRGBA(255, 255, 255, 252) : nvgRGBA(24, 26, 32, 252));
    }

    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto closedFlag = std::make_shared<std::atomic<bool>>(false);

    std::string cancelText = brls::getStr("app/common/cancel");
    if (cancelText.empty() || cancelText == "app/common/cancel") cancelText = "Cancel";

    // Borealis dismisses and frees the dialog after this callback.
    dialog->addButton(cancelText, [cancelFlag, closedFlag]() {
        cancelFlag->store(true);
        closedFlag->store(true);
    });

    dialog->open();

    brls::async([repo, appName, defaultFileName, destRelativePath, cancelFlag, closedFlag, dialog, statusLabel, progressFill, statsLabel, onComplete]() {
        auto lastUpdate = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        auto isSyncPending = std::make_shared<std::atomic<bool>>(false);

        auto onProgress = [lastUpdate, isSyncPending, statusLabel, progressFill, statsLabel, cancelFlag, closedFlag](const homebrew::InstallProgress& p) {
            if (cancelFlag->load()) return;
            if (isSyncPending->load(std::memory_order_relaxed)) return;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastUpdate).count();
            if (elapsed < 80 && p.fraction < 1.0f) return;
            *lastUpdate = now;
            isSyncPending->store(true, std::memory_order_relaxed);

            brls::sync([isSyncPending, statusLabel, progressFill, statsLabel, closedFlag, p]() {
                isSyncPending->store(false, std::memory_order_relaxed);
                if (closedFlag->load()) return;
                progressFill->setWidth(440.0f * std::clamp(p.fraction, 0.0f, 1.0f));
                int pct = static_cast<int>(std::clamp(p.fraction, 0.0f, 1.0f) * 100.0f);
                switch (p.stage) {
                    case homebrew::InstallProgress::Stage::Prepare:
                        statusLabel->setText("app/companion/preparing"_i18n);
                        break;
                    case homebrew::InstallProgress::Stage::Download:
                        statusLabel->setText(brls::getStr("app/companion/downloading", std::to_string(pct)));
                        statsLabel->setText(formatSizeMb(p.done) + (p.total > 0 ? " / " + formatSizeMb(p.total) : ""));
                        break;
                    case homebrew::InstallProgress::Stage::Finish:
                        statusLabel->setText("app/companion/moving"_i18n);
                        break;
                    default:
                        break;
                }
            });
        };

        std::string err;
        bool ok = catalog::installCompanionApp(repo, appName, defaultFileName, destRelativePath, onProgress, cancelFlag, err);
        bool wasCancelled = cancelFlag->load();

        brls::sync([dialog, closedFlag, appName, ok, wasCancelled, err, onComplete]() {
            auto finish = [appName, ok, wasCancelled, err, onComplete]() {
                if (wasCancelled) {
                    brls::Application::notify("app/companion/cancelled"_i18n);
                    if (onComplete) onComplete(false);
                } else if (!ok) {
                    std::string errStr = brls::getStr("app/companion/install_error") + ": " + err;
                    brls::Application::notify(errStr);
                    if (onComplete) onComplete(false);
                } else {
                    // Show success confirmation dialog
                    std::string title = brls::getStr("app/companion/success_title");
                    std::string msg = brls::getStr("app/companion/success_msg", appName);
                    auto* successDialog = new brls::Dialog(msg);
                    successDialog->addButton("app/common/ok"_i18n, [onComplete]() {
                        if (onComplete) onComplete(true);
                    });
                    successDialog->setCancelable(true);
                    successDialog->open();
                }
            };
            if (closedFlag->exchange(true)) finish();
            else dialog->close(finish);
        });
    });
}

static std::string formatPatchVersion(uint64_t appId, uint32_t v, const modcd::TitleMods& mods) {
    if (v == 0) {
        return "1.0.0 (v0)";
    }

    std::string displayVer;
    // 1. Static / cached mapping from modcd.json
    auto it = mods.patch_versions.find(v);
    if (it != mods.patch_versions.end() && !it->second.empty()) {
        displayVer = it->second;
    }

    // 2. Currently installed game on console (NACP display version)
    if (displayVer.empty()) {
        util::InstalledGameInfo info = util::queryInstalledGame(appId);
        if (info.installed && info.patchVersion == v && !info.displayVersion.empty()) {
            displayVer = info.displayVersion;
        }
    }

    // 3. Catalog releases
    if (displayVer.empty()) {
        auto snap = getCatalogSnapshot();
        if (snap) {
            for (const auto& g : *snap) {
                catalog::TitleVersion tv = catalog::titleVersion(g, appId);
                if ((tv.updateKnown || tv.update != 0) && tv.update == v && catalog::isPlausibleVersion(tv.version)) {
                    displayVer = catalog::trimVersion(tv.version);
                    break;
                }
            }
        }
    }

    if (!displayVer.empty()) {
        return displayVer + " (v" + std::to_string(v) + ")";
    }

    return "Update #" + std::to_string(catalog::updateOrdinal(v)) + " (v" + std::to_string(v) + ")";
}

void showModCdOfferDialog(uint64_t appId, const modcd::TitleMods& mods, std::function<void()> onDone) {
    std::string versionsStr;
    if (!mods.versions.empty()) {
        for (size_t i = 0; i < mods.versions.size(); ++i) {
            if (i > 0) versionsStr += ", ";
            versionsStr += formatPatchVersion(appId, mods.versions[i], mods);
        }
    }

    if (modcd::isModCdInstalled()) {
        std::string msg = brls::getStr("app/modcd/installed_info", std::to_string(mods.mods));
        if (!versionsStr.empty()) {
            msg += "\n\n" + "app/modcd/versions_supported"_i18n + ":\n" + versionsStr;
        }
        msg += "\n\n" + "app/modcd/launch_hint"_i18n;
        auto* dialog = new brls::Dialog(msg);
        dialog->addButton("app/common/ok"_i18n, [onDone]() { if (onDone) onDone(); });
        dialog->setCancelable(true);
        dialog->open();
    } else {
        std::string msg = brls::getStr("app/modcd/not_installed_prompt", std::to_string(mods.mods));
        if (!versionsStr.empty()) {
            msg += "\n\n" + "app/modcd/versions_supported"_i18n + ":\n" + versionsStr;
        }
        auto* dialog = new brls::Dialog(msg);
        dialog->addButton("app/modcd/install_now"_i18n, [onDone]() {
            showCompanionInstallDialog(modcd::kModCdRepo, "ModCD", "ModCD.nro", "switch/ModCD/ModCD.nro", [onDone](bool ok) {
                if (onDone) onDone();
            });
        });
        dialog->addButton("app/common/cancel"_i18n, [onDone]() { if (onDone) onDone(); });
        dialog->setCancelable(true);
        dialog->open();
    }
}

void showModKitOfferDialog(const util::SdkVersion& game_sdk,
                           const util::SdkVersion& console_sdk,
                           std::function<void()> onDone) {
    if (modcd::isModKitInstalled()) {
        std::string msg = brls::getStr("app/modkit/installed_info",
                                       game_sdk.toString(),
                                       console_sdk.toString());
        auto* dialog = new brls::Dialog(msg);
        dialog->addButton("app/common/ok"_i18n, [onDone]() { if (onDone) onDone(); });
        dialog->setCancelable(true);
        dialog->open();
    } else {
        std::string msg = brls::getStr("app/modkit/not_installed_prompt",
                                       game_sdk.toString(),
                                       console_sdk.toString());
        auto* dialog = new brls::Dialog(msg);
        dialog->addButton("app/modkit/install_now"_i18n, [onDone]() {
            showCompanionInstallDialog(modcd::kModKitRepo, "ModKitNX", "ModKitNX.nro", "switch/ModKitNX/ModKitNX.nro", [onDone](bool ok) {
                if (onDone) onDone();
            });
        });
        dialog->addButton("app/modkit/continue_without"_i18n, [onDone]() { if (onDone) onDone(); });
        dialog->setCancelable(true);
        dialog->open();
    }
}

} // namespace ui
