#include "HomebrewGlue.hpp"

#include "../../GameData.hpp"
#include "../../catalog/sysmodules.hpp"
#include "../../config/config.h"
#include "../../utils/app_paths.h"
#include "../../utils/log.h"
#include "../../utils/switch_utils.h"
#include "../RetroConsolesView.hpp"

#include <ctime>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace ui::hb {

void loadImage(brls::Image* img, const std::string& url, const std::shared_ptr<bool>& token, bool full_size, int row,
               int col) {
    setImageFromHTTPS(img, url, token, "romfs:/img/cover_blank.png", false, "", row, col, 0, full_size);
}

void haptic(int style) { util::triggerHapticFeedback(style); }

std::string currentSection() { return config::ConfigManager::instance().getHomebrewSection(); }

void rememberSection(const std::string& tab) { config::ConfigManager::instance().setHomebrewSection(tab); }

void showRetroSection() {
    rememberSection("retro");
    replaceTopActivity(new RetroConsolesView());
}

std::string today() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

void packagesChanged() {}

std::string sdRoot() { return TSNX_SD_ROOT; }

#ifdef __SWITCH__
bool canControlSysmodules() { return true; }

bool sysmoduleRunning(const std::string& tid) {
    const u64 id = homebrew::parseTitleId(tid);
    if (!id || R_FAILED(pmdmntInitialize())) return false;
    u64 pid = 0;
    const Result rc = pmdmntGetProcessId(&pid, id);
    pmdmntExit();
    return R_SUCCEEDED(rc) && pid != 0;
}

bool startSysmodule(const std::string& tid, std::string& err) {
    const u64 id = homebrew::parseTitleId(tid);
    if (!id) {
        err = "bad title id";
        return false;
    }
    if (R_FAILED(pmshellInitialize())) {
        err = "pm:shell unavailable";
        return false;
    }
    NcmProgramLocation loc{};
    loc.program_id = id;
    loc.storageID = NcmStorageId_None;
    u64 pid = 0;
    const Result rc = pmshellLaunchProgram(0, &loc, &pid);
    pmshellExit();
    if (R_FAILED(rc)) err = "launch failed: " + std::to_string(R_DESCRIPTION(rc));
    util::logLine("homebrew: start sysmodule " + tid + (R_SUCCEEDED(rc) ? " ok" : " failed " + err));
    return R_SUCCEEDED(rc);
}

bool stopSysmodule(const std::string& tid, std::string& err) {
    const u64 id = homebrew::parseTitleId(tid);
    if (!id || R_FAILED(pmshellInitialize())) {
        err = "pm:shell unavailable";
        return false;
    }
    const Result rc = pmshellTerminateProgram(id);
    pmshellExit();
    if (R_FAILED(rc)) err = "terminate failed: " + std::to_string(R_DESCRIPTION(rc));
    util::logLine("homebrew: stop sysmodule " + tid + (R_SUCCEEDED(rc) ? " ok" : " failed " + err));
    return R_SUCCEEDED(rc);
}
#else
bool canControlSysmodules() { return false; }
bool sysmoduleRunning(const std::string&) { return false; }
bool startSysmodule(const std::string&, std::string& err) {
    err = "not on a console";
    return false;
}
bool stopSysmodule(const std::string&, std::string& err) {
    err = "not on a console";
    return false;
}
#endif

}  // namespace ui::hb
