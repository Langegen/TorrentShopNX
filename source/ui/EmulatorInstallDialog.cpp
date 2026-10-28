#include "EmulatorInstallDialog.hpp"
#include "ThemeManager.hpp"
#include "../catalog/retro_emulator_manager.h"
#include "../catalog/emulator_installer.h"
#include "../utils/app_paths.h"
#include "../utils/log.h"
#include <borealis.hpp>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <atomic>
#include <memory>
#include <iomanip>
#include <sstream>

namespace ui {

namespace {

std::string formatSizeMb(int64_t bytes) {
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << mb << " MB";
    return ss.str();
}

} // namespace

void showEmulatorInstallDialog(const catalog::EmulatorPackage& pkg,
                               std::function<void(bool success)> onComplete) {
    bool isLight = ThemeManager::instance().isCurrentThemeLight();

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(480.0f);
    content->setPadding(20.0f);
    content->setAlignItems(brls::AlignItems::STRETCH);

    auto* titleLabel = new brls::Label();
    titleLabel->setText("app/retro/installing_title"_i18n + pkg.name);
    titleLabel->setFontSize(20.0f);
    titleLabel->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    titleLabel->setMarginBottom(8.0f);
    content->addView(titleLabel);

    auto* statusLabel = new brls::Label();
    statusLabel->setText("app/retro/preparing_download"_i18n);
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
    progressFill->setBackgroundColor(pkg.color);
    progressBg->addView(progressFill);
    content->addView(progressBg);

    auto* statsLabel = new brls::Label();
    statsLabel->setText("0.0 MB / " + formatSizeMb(pkg.file_size));
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

    dialog->addButton(cancelText, [cancelFlag, statusLabel]() {
        cancelFlag->store(true);
        statusLabel->setText("app/retro/cancelling_install"_i18n);
    });

    dialog->open();

    brls::async([pkg, cancelFlag, closedFlag, dialog, statusLabel, progressFill, statsLabel, onComplete]() {
        auto lastUpdate = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        auto isSyncPending = std::make_shared<std::atomic<bool>>(false);

        auto onProgress = [lastUpdate, isSyncPending, statusLabel, progressFill, statsLabel, cancelFlag](const catalog::InstallProgress& p) {
            if (cancelFlag->load()) return;
            if (isSyncPending->load(std::memory_order_relaxed)) return;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastUpdate).count();
            if (elapsed < 80 && p.fraction < 1.0f) return;
            *lastUpdate = now;
            isSyncPending->store(true, std::memory_order_relaxed);

            brls::sync([isSyncPending, statusLabel, progressFill, statsLabel, p]() {
                isSyncPending->store(false, std::memory_order_relaxed);
                progressFill->setWidth(440.0f * std::min(1.0f, p.fraction));
                int pct = static_cast<int>(std::min(1.0f, p.fraction) * 100.0f);
                switch (p.stage) {
                    case catalog::InstallProgress::Stage::Downloading:
                        if (p.file_count > 1) {
                            statusLabel->setText(brls::getStr("app/retro/downloading_comp_file", p.file,
                                                              std::to_string(p.file_index + 1), std::to_string(p.file_count)));
                            statsLabel->setText(p.file + ": " + formatSizeMb(p.done) +
                                                (p.total > 0 ? " / " + formatSizeMb(p.total) : std::string()));
                        } else {
                            statusLabel->setText(brls::getStr("app/retro/downloading_files_pct", std::to_string(pct)));
                            statsLabel->setText(formatSizeMb(p.done) +
                                                (p.total > 0 ? " / " + formatSizeMb(p.total) : std::string()));
                        }
                        break;
                    case catalog::InstallProgress::Stage::Extracting:
                        statusLabel->setText(brls::getStr("app/retro/extracting_pct", std::to_string(pct)));
                        if (!p.file.empty()) statsLabel->setText(p.file);
                        break;
                    case catalog::InstallProgress::Stage::Finishing:
                        statusLabel->setText("app/retro/moving_files"_i18n);
                        break;
                }
            });
        };

        std::string err;
        bool ok = catalog::installEmulatorPackage(pkg, onProgress, cancelFlag, err);
        bool wasCancelled = cancelFlag->load();

        brls::sync([dialog, closedFlag, pkg, ok, wasCancelled, err, onComplete]() {
            if (closedFlag->exchange(true)) return;
            dialog->close([pkg, ok, wasCancelled, err, onComplete]() {
                if (wasCancelled) {
                    brls::Application::notify("app/retro/install_cancelled"_i18n);
                } else if (!ok) {
                    if (err.rfind("extract", 0) == 0) {
                        brls::Application::notify("app/retro/extract_error_prefix"_i18n + err);
                    } else {
                        brls::Application::notify("app/retro/download_error_offline"_i18n);
                    }
                } else {
                    brls::Application::notify(brls::getStr("app/retro/emu_installed_success_format", pkg.name));
                }
                if (onComplete) onComplete(ok && !wasCancelled);
            });
        });
    });
}

void handlePostEmulatorInstallFlow(const catalog::EmulatorPackage& pkg,
                                   std::function<void()> onDone) {
    if (pkg.category == "bios" || pkg.id == "tico_bios") {
        if (onDone) onDone();
        return;
    }

    util::logLine("EmulatorInstall: handlePostEmulatorInstallFlow started for " + pkg.id);

    brls::sync([pkg, onDone]() {
        // Query the freshest package definition from manager
        const auto* freshPkg = catalog::RetroEmulatorManager::instance().findPackage(pkg.id);
        catalog::EmulatorPackage effectivePkg = freshPkg ? *freshPkg : pkg;

        // Check if emulator has an associated BIOS that is not yet installed
        const auto* biosPkg = catalog::RetroEmulatorManager::instance().getBiosPackageForEmulator(effectivePkg.id);
        if (biosPkg && !catalog::RetroEmulatorManager::instance().isInstalled(biosPkg->id)) {
            catalog::EmulatorPackage biosCopy = *biosPkg;
            util::logLine("EmulatorInstall: showing BIOS prompt for " + effectivePkg.id + " -> bios=" + biosCopy.id);
            std::string biosMsg = brls::getStr("app/retro/prompt_install_bios_msg", effectivePkg.name, biosCopy.name);
            auto* biosDlg = new brls::Dialog(biosMsg);
            biosDlg->addButton("app/retro/btn_install_bios"_i18n, [biosCopy, onDone]() {
                util::logLine("EmulatorInstall: user accepted BIOS install for " + biosCopy.id);
                brls::sync([biosCopy, onDone]() {
                    showEmulatorInstallDialog(biosCopy, [onDone](bool ok) {
                        if (onDone) onDone();
                    });
                });
            });
            biosDlg->addButton("app/retro/btn_skip"_i18n, [onDone]() {
                util::logLine("EmulatorInstall: user skipped BIOS install");
                if (onDone) onDone();
            });
            biosDlg->open();
        } else {
            if (onDone) onDone();
        }
    });
}

} // namespace ui
