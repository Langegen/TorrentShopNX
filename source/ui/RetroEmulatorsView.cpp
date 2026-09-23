#include "RetroEmulatorsView.hpp"
#include "EmulatorInstallDialog.hpp"
#include "../catalog/retro_emulator_manager.h"
#include "../catalog/retro_catalog_manager.h"
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
        statsHint->setText("app/retro/emu_stats_hint"_i18n);
    }

    // Register (-) to refresh manifest from GitHub
    this->registerAction("app/retro/action_refresh_list"_i18n, brls::ControllerButton::BUTTON_BACK, [this](brls::View* view) {
        refreshManifestOnline();
        return true;
    });

    rebuildList();
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
                brls::Application::notify("app/retro/emu_list_updated"_i18n);
                rebuildList();
            } else {
                brls::Application::notify("app/retro/emu_list_offline"_i18n);
            }
        });
    });
}

void RetroEmulatorsView::rebuildList() {
    if (!listBox) return;
    listBox->clearViews();

    auto& emuMgr = catalog::RetroEmulatorManager::instance();
    const auto& packages = emuMgr.getPackages();

    struct SectionDef {
        std::string category;
        std::string title;
        NVGcolor color;
    };

    std::vector<SectionDef> sections = {
        {"nintendo",  "app/retro/sec_emu_nintendo"_i18n,  nvgRGBA(230, 0, 18, 255)},
        {"sony",      "app/retro/sec_emu_sony"_i18n,      nvgRGBA(0, 55, 145, 255)},
        {"sega",      "app/retro/sec_emu_sega"_i18n,      nvgRGBA(0, 224, 165, 255)},
        {"retroarch", "app/retro/sec_emu_retroarch"_i18n, nvgRGBA(255, 152, 0, 255)},
        {"bios",      "app/retro/sec_emu_bios"_i18n,      nvgRGBA(233, 30, 99, 255)}
    };

    for (const auto& sec : sections) {
        std::vector<const catalog::EmulatorPackage*> secPkgs;
        for (const auto& p : packages) {
            if (p.category == sec.category) {
                secPkgs.push_back(&p);
            }
        }
        if (secPkgs.empty()) continue;

        listBox->addView(createCategoryHeader(sec.title, sec.color));

        for (const auto* pkgPtr : secPkgs) {
            const catalog::EmulatorPackage& pkg = *pkgPtr;

            auto* row = new brls::Box();
            row->setFocusable(true);
            row->setAxis(brls::Axis::ROW);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
            row->setWidthPercentage(100.0f);
            row->setPadding(12.0f, 16.0f, 12.0f, 16.0f);
            row->setMarginBottom(8.0f);
            row->setCornerRadius(8.0f);
            row->setBackgroundColor(nvgRGBA(32, 35, 42, 255));

            // --- Left info column ---
            auto* leftBox = new brls::Box();
            leftBox->setAxis(brls::Axis::COLUMN);
            leftBox->setGrow(1.0f);

            // Title row: Name + Version + Author
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
            nameLabel->setText(pkg.name);
            nameLabel->setFontSize(16.0f);
            nameLabel->setTextColor(nvgRGB(255, 255, 255));
            nameLabel->setMarginRight(10.0f);
            titleRow->addView(nameLabel);

            // Console badges (explicitly show which consoles this emulator runs)
            for (const auto& cid : pkg.supported_console_ids) {
                auto* cBadge = new brls::Box();
                cBadge->setPadding(2.0f, 7.0f, 2.0f, 7.0f);
                cBadge->setCornerRadius(4.0f);
                cBadge->setBackgroundColor(nvgRGBA(0, 180, 216, 40));
                cBadge->setMarginRight(6.0f);

                auto* cLbl = new brls::Label();
                cLbl->setText(getConsoleDisplayName(cid));
                cLbl->setFontSize(11.0f);
                cLbl->setTextColor(nvgRGB(56, 217, 245));
                cBadge->addView(cLbl);
                titleRow->addView(cBadge);
            }

            auto* verBadge = new brls::Box();
            verBadge->setPadding(2.0f, 6.0f, 2.0f, 6.0f);
            verBadge->setCornerRadius(4.0f);
            verBadge->setBackgroundColor(nvgRGBA(255, 255, 255, 25));
            verBadge->setMarginRight(8.0f);
            auto* verLbl = new brls::Label();
            verLbl->setText("v" + pkg.version);
            verLbl->setFontSize(11.0f);
            verLbl->setTextColor(nvgRGB(200, 200, 210));
            verBadge->addView(verLbl);
            titleRow->addView(verBadge);

            auto* authorLabel = new brls::Label();
            authorLabel->setText("app/retro/author_prefix"_i18n + pkg.author);
            authorLabel->setFontSize(12.0f);
            authorLabel->setTextColor(nvgRGB(140, 140, 150));
            titleRow->addView(authorLabel);

            leftBox->addView(titleRow);

            // Description
            auto* descLabel = new brls::Label();
            descLabel->setText(pkg.description);
            descLabel->setFontSize(12.0f);
            descLabel->setTextColor(nvgRGB(170, 170, 180));
            leftBox->addView(descLabel);

            row->addView(leftBox);

            // --- Right action & status column ---
            auto* rightBox = new brls::Box();
            rightBox->setAxis(brls::Axis::ROW);
            rightBox->setAlignItems(brls::AlignItems::CENTER);
            rightBox->setMarginLeft(16.0f);

            auto* sizeLabel = new brls::Label();
            sizeLabel->setText(formatSize(pkg.file_size));
            sizeLabel->setFontSize(13.0f);
            sizeLabel->setTextColor(nvgRGB(140, 140, 150));
            sizeLabel->setMarginRight(14.0f);
            rightBox->addView(sizeLabel);

            catalog::EmulatorInstallStatus status = emuMgr.getInstallStatus(pkg.id);

            auto* statusBadge = new brls::Box();
            statusBadge->setPadding(5.0f, 12.0f, 5.0f, 12.0f);
            statusBadge->setCornerRadius(5.0f);

            auto* badgeText = new brls::Label();
            badgeText->setFontSize(12.0f);

            if (status == catalog::EmulatorInstallStatus::INSTALLED) {
                statusBadge->setBackgroundColor(nvgRGBA(38, 166, 91, 40));
                badgeText->setTextColor(nvgRGB(46, 204, 113));
                badgeText->setText("app/retro/emulator_installed"_i18n);
            } else if (status == catalog::EmulatorInstallStatus::UPDATE_AVAILABLE) {
                statusBadge->setBackgroundColor(nvgRGBA(243, 156, 18, 40));
                badgeText->setTextColor(nvgRGB(241, 196, 15));
                badgeText->setText("app/retro/emulator_update_available"_i18n);
            } else {
                statusBadge->setBackgroundColor(nvgRGBA(0, 150, 214, 50));
                badgeText->setTextColor(nvgRGB(0, 190, 255));
                badgeText->setText("app/retro/emulator_download"_i18n);
            }
            statusBadge->addView(badgeText);
            rightBox->addView(statusBadge);

            row->addView(rightBox);

            // Click listener
            std::string emuId = pkg.id;
            catalog::EmulatorPackage curPkg = pkg;

            row->registerClickAction([this, curPkg, status](brls::View*) {
                const auto* freshPkg = catalog::RetroEmulatorManager::instance().findPackage(curPkg.id);
                catalog::EmulatorPackage activePkg = freshPkg ? *freshPkg : curPkg;

                if (status == catalog::EmulatorInstallStatus::INSTALLED) {
                    auto* chooseDialog = new brls::Dialog("app/retro/emu_prefix"_i18n + activePkg.name + "app/retro/emu_already_installed"_i18n);
                    chooseDialog->addButton("app/retro/btn_reinstall"_i18n, [this, activePkg]() {
                        brls::sync([this, activePkg]() {
                            showEmulatorInstallDialog(activePkg, [this, activePkg](bool ok) {
                                if (ok) {
                                    rebuildList();
                                    if (activePkg.category != "bios") {
                                        handlePostEmulatorInstallFlow(activePkg, [this]() { rebuildList(); });
                                    }
                                }
                            });
                        });
                    });

                    if (!activePkg.forwarder_url.empty()) {
                        chooseDialog->addButton("app/retro/btn_forwarder"_i18n, [this, activePkg]() {
                            brls::sync([this, activePkg]() {
                                installForwarderForEmulator(activePkg, [this](bool ok) {
                                    if (ok) rebuildList();
                                });
                            });
                        });
                    }

                    if (!activePkg.bios_id.empty()) {
                        const auto* biosPkg = catalog::RetroEmulatorManager::instance().findPackage(activePkg.bios_id);
                        if (biosPkg) {
                            catalog::EmulatorPackage biosCopy = *biosPkg;
                            chooseDialog->addButton("app/retro/btn_bios"_i18n, [this, biosCopy]() {
                                brls::sync([this, biosCopy]() {
                                    showEmulatorInstallDialog(biosCopy, [this](bool ok) {
                                        if (ok) rebuildList();
                                    });
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
                } else {
                    showEmulatorInstallDialog(activePkg, [this, activePkg](bool ok) {
                        if (ok) {
                            rebuildList();
                            if (activePkg.category != "bios") {
                                handlePostEmulatorInstallFlow(activePkg, [this]() { rebuildList(); });
                            }
                        }
                    });
                }
                return true;
            });

            // Action BUTTON_X for quick uninstall
            if (status == catalog::EmulatorInstallStatus::INSTALLED || status == catalog::EmulatorInstallStatus::UPDATE_AVAILABLE) {
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

            listBox->addView(row);
        }
    }
}

} // namespace ui
