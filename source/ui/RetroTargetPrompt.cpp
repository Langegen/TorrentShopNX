#include "RetroTargetPrompt.hpp"
#include "../config/config.h"
#include "../catalog/retro_catalog_manager.h"
#include "../utils/app_paths.h"
#include <borealis.hpp>

namespace ui {

void promptRetroTargetSelection(const std::string& retro_console_id,
                                std::function<void(const std::string& target_mode)> onChosen) {
    if (retro_console_id.empty()) {
        if (onChosen) onChosen("");
        return;
    }

    auto& cfg = config::ConfigManager::instance();
    std::string mode = cfg.getRetroRomsMode();

    if (mode == "tico") {
        if (onChosen) onChosen("tico");
        return;
    }
    if (mode == "retroarch") {
        if (onChosen) onChosen("standalone");
        return;
    }
    if (mode == "downloads") {
        if (onChosen) onChosen("downloads");
        return;
    }
    if (mode == "custom") {
        if (onChosen) onChosen("custom");
        return;
    }

    // Default: "prompt" mode - Ask user
    bool isTicoSupported = catalog::RetroCatalogManager::instance().isTicoSupported(retro_console_id);
    std::string ticoSlug = catalog::RetroCatalogManager::instance().getTicoSlug(retro_console_id);
    std::string ticoPath = std::string(TSNX_TICO_ROMS_DIR) + "/" + ticoSlug + "/";

    std::string stdSubfolder = catalog::RetroCatalogManager::instance().getEffectiveConsoleSubfolder(retro_console_id, false);
    std::string stdPath = cfg.getEffectiveRetroRomsDir(stdSubfolder);
    if (!stdPath.empty() && stdPath.back() != '/') stdPath += '/';

    std::string desc;
    if (isTicoSupported) {
        desc = brls::getStr("app/retro/target_dialog_desc", ticoPath, stdPath);
    } else {
        desc = brls::getStr("app/retro/target_dialog_desc_unsupported", ticoPath, stdPath);
    }

    std::string title = brls::getStr("app/retro/target_dialog_title");
    std::string fullMsg = !title.empty() ? (title + "\n\n" + desc) : desc;

    auto* dialog = new brls::Dialog(fullMsg);
    dialog->setCancelable(true);

    dialog->addButton("app/retro/target_btn_tico"_i18n, [onChosen]() {
        if (onChosen) onChosen("tico");
    });

    dialog->addButton("app/retro/target_btn_standalone"_i18n, [onChosen]() {
        if (onChosen) onChosen("standalone");
    });

    dialog->addButton("app/common/cancel"_i18n, []() {});

    dialog->open();
}

} // namespace ui
