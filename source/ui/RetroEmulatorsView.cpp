#include "RetroEmulatorsView.hpp"
#include "EmulatorInstallDialog.hpp"
#include "ThemeManager.hpp"
#include "../catalog/retro_emulator_manager.h"
#include "../catalog/retro_catalog_manager.h"
#include "../catalog/retro_readiness.h"
#include "../config/config.h"
#include "../utils/log.h"
#include <iomanip>
#include <sstream>

namespace ui {

namespace {

static std::string getConsoleDisplayName(const std::string& cid) {
    if (cid == "3ds") return "3DS";
    if (cid == "ps2") return "PS2";
    if (cid == "gamecube") return "GameCube";
    if (cid == "wii") return "Wii";
    if (cid == "psvita") return "PS Vita";
    if (cid == "wiiu") return "Wii U";
    if (cid == "nds") return "NDS";
    if (cid == "ps1") return "PS1";
    if (cid == "psp") return "PSP";
    if (cid == "dreamcast") return "Dreamcast";
    if (cid == "snes") return "SNES";
    if (cid == "nes") return "NES";
    if (cid == "sega_md") return "Mega Drive";
    if (cid == "sega_ms") return "Master System";
    if (cid == "sega_gg") return "Game Gear";
    if (cid == "sega_cd") return "Sega CD";
    if (cid == "gba") return "GBA";
    if (cid == "gbc") return "GBC";
    if (cid == "n64") return "N64";
    if (cid == "sega_32x") return "Sega 32X";
    const auto* c = catalog::RetroCatalogManager::instance().findConsole(cid);
    return c ? c->name : cid;
}

std::string formatSize(int64_t bytes) {
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << mb << " MB";
    return ss.str();
}

brls::Box* createCategoryHeader(const std::string& title, NVGcolor color) {
    auto* box = new brls::Box();
    box->setFocusable(false);
    box->setAxis(brls::Axis::ROW);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setWidthPercentage(100.0f);
    box->setMarginTop(14.0f);
    box->setMarginBottom(10.0f);

    auto* bar = new brls::Box();
    bar->setWidth(4.0f);
    bar->setHeight(18.0f);
    bar->setCornerRadius(2.0f);
    bar->setBackgroundColor(color);
    bar->setMarginRight(8.0f);
    box->addView(bar);

    auto* lbl = new brls::Label();
    lbl->setText(title);
    lbl->setFontSize(14.0f);
    lbl->setTextColor(color);
    box->addView(lbl);

    return box;
}

} // namespace

RetroEmulatorsView::RetroEmulatorsView()
    : alive_flag_(std::make_shared<std::atomic<bool>>(true)) {
}

RetroEmulatorsView::~RetroEmulatorsView() {
    *alive_flag_ = false;
}

void RetroEmulatorsView::onContentAvailable() {
    if (scroll) {
        scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    }

    if (titleLabel) {
        titleLabel->setText("app/retro/emulators_title"_i18n);
    }
    if (statsHint) {
        statsHint->setText("app/retro/emulators_subtitle"_i18n);
    }

    // Register (-) to refresh manifest from GitHub
    this->registerAction("app/retro/action_refresh_list"_i18n, brls::ControllerButton::BUTTON_BACK, [this](brls::View* view) {
        refreshManifestOnline();
        return true;
    });

    rebuildList();

    // Daily automatic background refresh of emulator manifest
    if (config::ConfigManager::instance().shouldCheckEmulatorManifestToday()) {
        auto flag = alive_flag_;
        brls::async([this, flag]() {
            bool ok = catalog::RetroEmulatorManager::instance().refreshManifest();
            if (!flag->load()) return;
            if (ok) {
                config::ConfigManager::instance().setLastEmulatorManifestCheckDate(
                    config::ConfigManager::currentDateString()
                );
                brls::sync([this, flag]() {
                    if (!flag->load()) return;
                    rebuildList();
                });
            }
        });
    }
}

void RetroEmulatorsView::refreshManifestOnline() {
    auto flag = alive_flag_;
    brls::Application::notify("app/retro/checking_emu_updates"_i18n);

    brls::async([this, flag]() {
        bool ok = catalog::RetroEmulatorManager::instance().refreshManifest();
        if (!flag->load()) return;

        brls::sync([this, flag, ok]() {
            if (!flag->load()) return;
            if (ok) {
                config::ConfigManager::instance().setLastEmulatorManifestCheckDate(
                    config::ConfigManager::currentDateString()
                );
                brls::Application::notify("app/retro/emu_list_updated"_i18n);
                rebuildList();
            } else {
                brls::Application::notify("app/retro/emu_list_offline"_i18n);
            }
        });
    });
}

namespace {

brls::Box* makeBadge(const std::string& text, NVGcolor bg, NVGcolor fg) {
    auto* badge = new brls::Box();
    badge->setPadding(2.0f, 7.0f, 2.0f, 7.0f);
    badge->setCornerRadius(4.0f);
    badge->setBackgroundColor(bg);
    badge->setMarginRight(6.0f);
    badge->setShrink(0.0f);
    auto* lbl = new brls::Label();
    lbl->setText(text);
    lbl->setFontSize(11.0f);
    lbl->setTextColor(fg);
    lbl->setSingleLine(true);
    badge->addView(lbl);
    return badge;
}

brls::Box* makeRowShell(bool isLight) {
    auto* row = new brls::Box();
    row->setFocusable(true);
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    row->setWidthPercentage(100.0f);
    row->setPadding(12.0f, 16.0f, 12.0f, 16.0f);
    row->setMarginBottom(8.0f);
    row->setCornerRadius(8.0f);
    if (isLight) {
        row->setBackgroundColor(nvgRGBA(245, 247, 250, 255));
        row->setBorderThickness(1.0f);
        row->setBorderColor(nvgRGBA(215, 222, 232, 255));
    } else {
        row->setBackgroundColor(nvgRGBA(32, 35, 42, 255));
    }
    return row;
}

// Green "installed", amber "update" / "missing", blue "download".
brls::Box* makeStatusBadge(const std::string& text, int kind, bool isLight) {
    NVGcolor bg, fg;
    switch (kind) {
        case 0: // ok
            bg = isLight ? nvgRGBA(38, 166, 91, 35) : nvgRGBA(38, 166, 91, 40);
            fg = isLight ? nvgRGB(22, 128, 61) : nvgRGB(46, 204, 113);
            break;
        case 1: // attention
            bg = isLight ? nvgRGBA(234, 88, 12, 35) : nvgRGBA(243, 156, 18, 40);
            fg = isLight ? nvgRGB(194, 65, 12) : nvgRGB(241, 196, 15);
            break;
        case 2: // neutral
            bg = isLight ? nvgRGBA(0, 0, 0, 12) : nvgRGBA(255, 255, 255, 25);
            fg = isLight ? nvgRGB(100, 112, 128) : nvgRGB(200, 200, 210);
            break;
        default: // action
            bg = isLight ? nvgRGBA(0, 150, 214, 35) : nvgRGBA(0, 150, 214, 50);
            fg = isLight ? nvgRGB(2, 132, 199) : nvgRGB(0, 190, 255);
            break;
    }
    auto* badge = new brls::Box();
    badge->setPadding(5.0f, 12.0f, 5.0f, 12.0f);
    badge->setCornerRadius(5.0f);
    badge->setShrink(0.0f);
    badge->setBackgroundColor(bg);
    auto* lbl = new brls::Label();
    lbl->setText(text);
    lbl->setFontSize(12.0f);
    lbl->setSingleLine(true);
    lbl->setTextColor(fg);
    badge->addView(lbl);
    return badge;
}

bool isStandaloneRunner(const catalog::EmulatorPackage& p) {
    return p.id == "vita3k" || p.id == "cemu";
}

} // namespace

brls::Box* RetroEmulatorsView::buildPackageRow(const catalog::EmulatorPackage& pkg, bool isLight) {
    auto& emuMgr = catalog::RetroEmulatorManager::instance();
    auto* row = makeRowShell(isLight);

    std::string emuId = pkg.id;
    row->getFocusEvent()->subscribe([this, emuId](brls::View* v) {
        if (v && v->isFocused()) lastFocusedPkgId_ = emuId;
    });

    // --- Left: name, consoles, version, author, description ---
    auto* leftBox = new brls::Box();
    leftBox->setAxis(brls::Axis::COLUMN);
    leftBox->setGrow(1.0f);
    leftBox->setShrink(1.0f);

    auto* titleRow = new brls::Box();
    titleRow->setAxis(brls::Axis::ROW);
    titleRow->setAlignItems(brls::AlignItems::CENTER);
    titleRow->setMarginBottom(4.0f);

    auto* colorDot = new brls::Box();
    colorDot->setWidth(8.0f);
    colorDot->setHeight(8.0f);
    colorDot->setCornerRadius(4.0f);
    colorDot->setBackgroundColor(pkg.color);
    colorDot->setMarginRight(8.0f);
    titleRow->addView(colorDot);

    auto* nameLabel = new brls::Label();
    nameLabel->setText(pkg.id == "tico_bios" ? "app/retro/tico_bios_title"_i18n : pkg.name);
    nameLabel->setFontSize(16.0f);
    nameLabel->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    nameLabel->setMarginRight(10.0f);
    titleRow->addView(nameLabel);

    NVGcolor consoleBg = isLight ? nvgRGBA(0, 150, 214, 25) : nvgRGBA(0, 180, 216, 40);
    NVGcolor consoleFg = isLight ? nvgRGB(2, 132, 199) : nvgRGB(56, 217, 245);
    if (pkg.supported_console_ids.size() > 4) {
        titleRow->addView(makeBadge(brls::getStr("app/retro/tico_systems_badge_count",
                                                 std::to_string(pkg.supported_console_ids.size())),
                                    isLight ? nvgRGBA(124, 77, 255, 30) : nvgRGBA(124, 77, 255, 45),
                                    isLight ? nvgRGB(100, 45, 220) : nvgRGB(180, 150, 255)));
    } else {
        for (const auto& cid : pkg.supported_console_ids) {
            titleRow->addView(makeBadge(getConsoleDisplayName(cid), consoleBg, consoleFg));
        }
    }

    std::string installedVer = emuMgr.getInstalledVersion(pkg.id);
    std::string shownVer = installedVer.empty() ? pkg.version : installedVer;
    if (!shownVer.empty() && shownVer != "latest") {
        titleRow->addView(makeBadge("v" + shownVer,
                                    isLight ? nvgRGBA(0, 0, 0, 12) : nvgRGBA(255, 255, 255, 25),
                                    isLight ? nvgRGB(100, 112, 128) : nvgRGB(200, 200, 210)));
    }

    if (pkg.category != "tico_module") {
        auto* authorLabel = new brls::Label();
        authorLabel->setText("app/retro/author_prefix"_i18n + pkg.author);
        authorLabel->setFontSize(12.0f);
        authorLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        titleRow->addView(authorLabel);
    }
    leftBox->addView(titleRow);

    auto* descLabel = new brls::Label();
    if (pkg.id == "tico") {
        descLabel->setText("app/retro/tico_emu_desc_modules"_i18n);
    } else if (pkg.category == "tico_module") {
        descLabel->setText("app/retro/tico_module_desc"_i18n);
    } else if (pkg.id == "tico_bios") {
        descLabel->setText("app/retro/tico_bios_desc"_i18n);
    } else {
        descLabel->setText(pkg.description);
    }
    descLabel->setFontSize(12.0f);
    descLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    descLabel->setSingleLine(true);
    leftBox->addView(descLabel);
    row->addView(leftBox);

    // --- Right: size and status ---
    auto* rightBox = new brls::Box();
    rightBox->setAxis(brls::Axis::ROW);
    rightBox->setAlignItems(brls::AlignItems::CENTER);
    rightBox->setMarginLeft(16.0f);
    rightBox->setShrink(0.0f);

    auto* sizeLabel = new brls::Label();
    sizeLabel->setText(formatSize(pkg.file_size));
    sizeLabel->setFontSize(13.0f);
    sizeLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    sizeLabel->setMarginRight(14.0f);
    sizeLabel->setSingleLine(true);
    rightBox->addView(sizeLabel);

    catalog::EmulatorInstallStatus status = emuMgr.getInstallStatus(pkg.id);
    if (status == catalog::EmulatorInstallStatus::INSTALLED) {
        rightBox->addView(makeStatusBadge("app/retro/emulator_installed"_i18n, 0, isLight));
    } else if (status == catalog::EmulatorInstallStatus::UPDATE_AVAILABLE) {
        rightBox->addView(makeStatusBadge("app/retro/emulator_update_available"_i18n, 1, isLight));
    } else {
        rightBox->addView(makeStatusBadge("app/retro/emulator_download"_i18n, 3, isLight));
    }
    row->addView(rightBox);

    // --- Actions ---
    catalog::EmulatorPackage curPkg = pkg;
    row->registerClickAction([this, curPkg, status](brls::View*) {
        catalog::EmulatorPackage activePkg = curPkg;
        catalog::RetroEmulatorManager::instance().getPackageCopy(curPkg.id, activePkg);

        auto afterInstall = [this, activePkg](bool ok) {
            if (!ok) return;
            rebuildList();
            if (activePkg.category != "bios" && activePkg.category != "tico_module") {
                handlePostEmulatorInstallFlow(activePkg, [this]() { rebuildList(); });
            }
        };

        if (status == catalog::EmulatorInstallStatus::NOT_INSTALLED) {
            showEmulatorInstallDialog(activePkg, afterInstall);
            return true;
        }

        auto* chooseDialog = new brls::Dialog("app/retro/emu_prefix"_i18n + activePkg.name + "app/retro/emu_already_installed"_i18n);
        chooseDialog->addButton(status == catalog::EmulatorInstallStatus::UPDATE_AVAILABLE
                                    ? "app/retro/btn_update"_i18n : "app/retro/btn_reinstall"_i18n,
                                [activePkg, afterInstall]() {
            brls::sync([activePkg, afterInstall]() { showEmulatorInstallDialog(activePkg, afterInstall); });
        });

        if (!activePkg.bios_id.empty()) {
            catalog::EmulatorPackage biosPkg;
            if (catalog::RetroEmulatorManager::instance().getPackageCopy(activePkg.bios_id, biosPkg)) {
                chooseDialog->addButton("app/retro/btn_bios"_i18n, [this, biosPkg]() {
                    brls::sync([this, biosPkg]() {
                        showEmulatorInstallDialog(biosPkg, [this](bool ok) { if (ok) rebuildList(); });
                    });
                });
            }
        }

        chooseDialog->addButton("app/retro/btn_delete"_i18n, [this, activePkg]() {
            std::string err;
            if (catalog::RetroEmulatorManager::instance().uninstallEmulator(activePkg.id, err)) {
                brls::Application::notify("app/retro/emu_deleted"_i18n);
                rebuildList();
            } else {
                brls::Application::notify("app/retro/emu_delete_error"_i18n + err);
            }
        });
        chooseDialog->addButton("app/common/cancel"_i18n, []() {});
        chooseDialog->open();
        return true;
    });

    if (status != catalog::EmulatorInstallStatus::NOT_INSTALLED) {
        row->registerAction("app/retro/btn_delete"_i18n, brls::ControllerButton::BUTTON_X, [this, curPkg](brls::View*) {
            auto* confirmDialog = new brls::Dialog("app/retro/confirm_delete_emu_title"_i18n + curPkg.name + "app/retro/confirm_delete_emu_suffix"_i18n);
            confirmDialog->addButton("app/retro/btn_delete"_i18n, [this, curPkg]() {
                std::string err;
                if (catalog::RetroEmulatorManager::instance().uninstallEmulator(curPkg.id, err)) {
                    brls::Application::notify("app/retro/emu_deleted"_i18n);
                    rebuildList();
                } else {
                    brls::Application::notify("app/retro/emu_delete_error"_i18n + err);
                }
            });
            confirmDialog->addButton("app/common/cancel"_i18n, []() {});
            confirmDialog->open();
            return true;
        });
    }
    return row;
}

brls::Box* RetroEmulatorsView::buildSystemFileRow(const std::string& console_id, const std::string& hint_key,
                                                  const std::string& dir, bool present, bool optional, bool isLight) {
    auto* row = makeRowShell(isLight);
    std::string focusKey = "sys:" + console_id;
    row->getFocusEvent()->subscribe([this, focusKey](brls::View* v) {
        if (v && v->isFocused()) lastFocusedPkgId_ = focusKey;
    });

    auto* leftBox = new brls::Box();
    leftBox->setAxis(brls::Axis::COLUMN);
    leftBox->setGrow(1.0f);
    leftBox->setShrink(1.0f);

    const auto* cInfo = catalog::RetroCatalogManager::instance().findConsole(console_id);
    auto* nameLabel = new brls::Label();
    nameLabel->setText(cInfo ? cInfo->name : console_id);
    nameLabel->setFontSize(16.0f);
    nameLabel->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    nameLabel->setMarginBottom(4.0f);
    leftBox->addView(nameLabel);

    auto* hintLabel = new brls::Label();
    hintLabel->setText(brls::getStr(hint_key) + "  →  " + dir);
    hintLabel->setFontSize(12.0f);
    hintLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    hintLabel->setSingleLine(true);
    leftBox->addView(hintLabel);
    row->addView(leftBox);

    auto* rightBox = new brls::Box();
    rightBox->setMarginLeft(16.0f);
    rightBox->setShrink(0.0f);
    if (present) {
        rightBox->addView(makeStatusBadge("app/retro/sysfile_found"_i18n, 0, isLight));
    } else if (optional) {
        rightBox->addView(makeStatusBadge("app/retro/sysfile_optional"_i18n, 2, isLight));
    } else {
        rightBox->addView(makeStatusBadge("app/retro/sysfile_missing"_i18n, 1, isLight));
    }
    row->addView(rightBox);

    // The hint line is cut on narrow screens: A shows it whole.
    std::string title = cInfo ? cInfo->name : console_id;
    std::string text = brls::getStr(hint_key) + "\n\n" + dir;
    row->registerClickAction([title, text](brls::View*) {
        auto* dialog = new brls::Dialog(title + "\n\n" + text);
        dialog->addButton("app/common/ok"_i18n, []() {});
        dialog->open();
        return true;
    });
    return row;
}

brls::Box* RetroEmulatorsView::buildActionRow(const std::string& text, const std::string& detail, bool isLight) {
    auto* row = makeRowShell(isLight);
    auto* lbl = new brls::Label();
    lbl->setText(text);
    lbl->setFontSize(15.0f);
    lbl->setTextColor(ThemeManager::instance().getAccentColor());
    lbl->setGrow(1.0f);
    lbl->setShrink(1.0f);
    row->addView(lbl);
    if (!detail.empty()) {
        auto* d = new brls::Label();
        d->setText(detail);
        d->setFontSize(13.0f);
        d->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        d->setMarginLeft(16.0f);
        d->setSingleLine(true);
        row->addView(d);
    }
    return row;
}

void RetroEmulatorsView::installSequence(std::vector<std::string> package_ids, size_t index) {
    if (index >= package_ids.size()) {
        rebuildList();
        return;
    }
    catalog::EmulatorPackage pkg;
    if (!catalog::RetroEmulatorManager::instance().getPackageCopy(package_ids[index], pkg)) {
        installSequence(std::move(package_ids), index + 1);
        return;
    }
    auto flag = alive_flag_;
    showEmulatorInstallDialog(pkg, [this, flag, package_ids, index](bool ok) {
        if (!flag->load()) return;
        if (!ok) {
            rebuildList();
            return; // stop on failure or cancel
        }
        brls::sync([this, flag, package_ids, index]() {
            if (flag->load()) installSequence(package_ids, index + 1);
        });
    });
}

void RetroEmulatorsView::rebuildList() {
    if (!listBox) return;

    bool hadFocusInside = false;
    brls::View* currFocus = brls::Application::getCurrentFocus();
    if (currFocus) {
        brls::View* content = this->getContentView();
        for (brls::View* p = currFocus; p; p = p->getParent()) {
            if (p == listBox || p == content) {
                hadFocusInside = true;
                break;
            }
        }
    } else {
        hadFocusInside = true;
    }

    listBox->clearViews();

    auto& emuMgr = catalog::RetroEmulatorManager::instance();
    const auto& packages = emuMgr.getPackages();
    bool isLight = ThemeManager::instance().isCurrentThemeLight();

    brls::View* targetFocusView = nullptr;
    brls::View* firstFocusableRow = nullptr;
    auto addRow = [&](brls::Box* row, const std::string& key) {
        if (!firstFocusableRow) firstFocusableRow = row;
        if (!lastFocusedPkgId_.empty() && key == lastFocusedPkgId_) targetFocusView = row;
        listBox->addView(row);
    };

    // 1. Tico and its modules
    NVGcolor ticoColor = isLight ? nvgRGB(100, 45, 220) : nvgRGBA(140, 95, 255, 255);
    listBox->addView(createCategoryHeader("app/retro/sec_tico_modules"_i18n, ticoColor));
    if (const auto* tico = emuMgr.findPackage("tico")) {
        addRow(buildPackageRow(*tico, isLight), tico->id);
    }

    std::vector<std::string> missingModules;
    int64_t missingBytes = 0;
    for (const auto& p : packages) {
        if (p.category != "tico_module") continue;
        if (!emuMgr.isInstalled(p.id)) {
            missingModules.push_back(p.id);
            missingBytes += p.file_size;
        }
    }
    if (!missingModules.empty()) {
        auto* row = buildActionRow(brls::getStr("app/retro/install_missing_modules", std::to_string(missingModules.size())),
                                   formatSize(missingBytes), isLight);
        std::vector<std::string> toInstall;
        if (!emuMgr.isInstalled("tico") || emuMgr.getInstallStatus("tico") == catalog::EmulatorInstallStatus::UPDATE_AVAILABLE) {
            toInstall.push_back("tico");
        }
        toInstall.insert(toInstall.end(), missingModules.begin(), missingModules.end());
        row->registerClickAction([this, toInstall](brls::View*) {
            installSequence(toInstall, 0);
            return true;
        });
        addRow(row, "action:modules");
    }
    for (const auto& p : packages) {
        if (p.category == "tico_module") addRow(buildPackageRow(p, isLight), p.id);
    }

    // 2. Standalone emulators for the consoles Tico does not run
    listBox->addView(createCategoryHeader("app/retro/sec_standalone_emus"_i18n,
                                          isLight ? nvgRGB(2, 132, 199) : nvgRGBA(33, 150, 243, 255)));
    for (const char* id : {"vita3k", "cemu"}) {
        if (const auto* p = emuMgr.findPackage(id)) addRow(buildPackageRow(*p, isLight), p->id);
    }

    // 3. System files the emulators ask for
    listBox->addView(createCategoryHeader("app/retro/sec_system_files"_i18n,
                                          isLight ? nvgRGB(190, 24, 93) : nvgRGBA(233, 30, 99, 255)));
    for (const auto& f : catalog::checkAllSystemFiles()) {
        addRow(buildSystemFileRow(f.console_id, f.hint_key, f.dir, f.present, f.optional, isLight), "sys:" + f.console_id);
    }

    // 4. Everything else, folded by default
    std::vector<const catalog::EmulatorPackage*> others;
    for (const auto& p : packages) {
        if (p.id == "tico" || p.category == "tico_module" || isStandaloneRunner(p)) continue;
        others.push_back(&p);
    }
    if (!others.empty()) {
        listBox->addView(createCategoryHeader("app/retro/sec_other_emus"_i18n,
                                              isLight ? nvgRGB(100, 112, 128) : nvgRGBA(160, 165, 175, 255)));
        auto* toggle = buildActionRow(showOthers_ ? "app/retro/hide_other_emus"_i18n
                                                  : brls::getStr("app/retro/show_other_emus", std::to_string(others.size())),
                                      "", isLight);
        toggle->registerClickAction([this](brls::View*) {
            showOthers_ = !showOthers_;
            lastFocusedPkgId_ = "action:others";
            rebuildList();
            return true;
        });
        addRow(toggle, "action:others");
        if (showOthers_) {
            for (const auto* p : others) addRow(buildPackageRow(*p, isLight), p->id);
        }
    }

    if (statsHint) {
        size_t installed = 0, total = 0;
        for (const auto& p : packages) {
            if (p.id != "tico" && p.category != "tico_module" && !isStandaloneRunner(p)) continue;
            ++total;
            if (emuMgr.isInstalled(p.id)) ++installed;
        }
        statsHint->setText("app/retro/emulators_subtitle"_i18n + " (" + std::to_string(installed) + "/" + std::to_string(total) + ")");
    }

    if (hadFocusInside) {
        brls::View* viewToFocus = targetFocusView ? targetFocusView : firstFocusableRow;
        if (viewToFocus) {
            brls::sync([viewToFocus]() { brls::Application::giveFocus(viewToFocus); });
        }
    }
}

} // namespace ui
