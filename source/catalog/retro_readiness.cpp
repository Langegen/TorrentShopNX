#include "retro_readiness.h"

#include "retro_catalog_manager.h"
#include "retro_emulator_manager.h"
#include "../utils/log.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace catalog {

namespace {

std::string local(const std::string& sdmc_path) {
    return RetroEmulatorManager::resolvePlatformPath(sdmc_path);
}

bool fileWithSize(const std::string& path, uintmax_t size) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && fs::file_size(path, ec) == size;
}

// Any file of the given size in dir (DuckStation and NetherSX2 accept any name).
bool anyFileOfSize(const std::string& dir, uintmax_t size) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code ec2;
        if (e.is_regular_file(ec2) && e.file_size(ec2) == size) return true;
    }
    return false;
}

bool anyExists(const std::string& dir, const std::vector<std::string>& names) {
    std::error_code ec;
    for (const auto& n : names) {
        if (fs::is_regular_file(dir + "/" + n, ec)) return true;
    }
    return false;
}

bool dirHasExtension(const std::string& dir, const std::string& ext) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string x = e.path().extension().string();
        std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (x == ext) return true;
    }
    return false;
}

// Flycast can run without dc_boot.bin when "HLE BIOS" is on in Tico
// (sdmc:/tico/config/cores/flycast.jsonc, key reicast_hle_bios).
bool flycastUsesHleBios() {
    std::ifstream in(local("sdmc:/tico/config/cores/flycast.jsonc"));
    if (!in.is_open()) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string body = ss.str();
    size_t key = body.find("\"reicast_hle_bios\"");
    if (key == std::string::npos) return false;
    size_t colon = body.find(':', key);
    if (colon == std::string::npos) return false;
    size_t q1 = body.find('"', colon);
    if (q1 == std::string::npos) return false;
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    return body.substr(q1 + 1, q2 - q1 - 1) == "enabled";
}

// What each console needs besides the emulator. Rules follow the Tico wiki
// ("BIOS setup guide") and the Vita3K-nx / Cemu-nx launchers.
std::vector<SystemFileCheck> systemFilesFor(const std::string& console_id) {
    std::vector<SystemFileCheck> out;
    auto add = [&](const char* key, const char* dir, bool present, bool optional = false) {
        out.push_back({console_id, key, dir, present, optional});
    };

    if (console_id == "ps1") {
        add("app/retro/bios_hint_ps1", "sdmc:/tico/system/duckstation/",
            anyFileOfSize(local("sdmc:/tico/system/duckstation"), 512 * 1024));
    } else if (console_id == "ps2") {
        add("app/retro/bios_hint_ps2", "sdmc:/tico/system/nethersx2/",
            anyFileOfSize(local("sdmc:/tico/system/nethersx2"), 4 * 1024 * 1024));
    } else if (console_id == "dreamcast") {
        add("app/retro/bios_hint_dc", "sdmc:/tico/system/flycast/",
            fileWithSize(local("sdmc:/tico/system/flycast/dc_boot.bin"), 2 * 1024 * 1024) || flycastUsesHleBios());
    } else if (console_id == "sega_cd") {
        add("app/retro/bios_hint_segacd", "sdmc:/tico/system/genesis_plus_gx/",
            anyExists(local("sdmc:/tico/system/genesis_plus_gx"), {"bios_CD_U.bin", "bios_CD_E.bin", "bios_CD_J.bin"}));
    } else if (console_id == "nds") {
        std::string d = local("sdmc:/tico/system/drastic");
        bool ok = anyExists(d, {"nds_bios_arm7.bin", "bios7.bin"}) &&
                  anyExists(d, {"nds_bios_arm9.bin", "bios9.bin"}) &&
                  anyExists(d, {"nds_firmware.bin", "firmware.bin"});
        add("app/retro/bios_hint_nds", "sdmc:/tico/system/drastic/", ok);
    } else if (console_id == "psvita") {
        // Vita3K-nx treats the firmware as installed when its system modules are unpacked.
        add("app/retro/vita_fw_hint", "sdmc:/switch/vita3k/",
            dirHasExtension(local("sdmc:/switch/vita3k/vita/vs0/sys/external"), ".suprx"));
    } else if (console_id == "wiiu") {
        std::error_code ec;
        add("app/retro/cemu_keys_hint", "sdmc:/switch/cemu/",
            fs::is_regular_file(local("sdmc:/switch/cemu/keys.txt"), ec), true);
    }
    return out;
}

} // namespace

bool RetroReadiness::emulatorReady() const {
    if (!runner_installed || runner_outdated) return false;
    return module_pkg.empty() || module_installed;
}

bool RetroReadiness::systemFilesReady() const {
    return std::all_of(system_files.begin(), system_files.end(),
                       [](const SystemFileCheck& f) { return f.present || f.optional; });
}

RetroReadiness checkRetroReadiness(const std::string& console_id) {
    RetroReadiness r;
    auto& cat = RetroCatalogManager::instance();
    auto& emu = RetroEmulatorManager::instance();
    const RetroConsoleInfo* info = cat.findConsole(console_id);

    r.runner_pkg = cat.getRunnerPackageId(console_id);
    r.runner_installed = emu.isInstalled(r.runner_pkg);
    if (r.runner_installed && r.runner_pkg == "tico") {
        std::string v = emu.getInstalledVersion("tico");
        r.runner_outdated = !v.empty() && RetroEmulatorManager::isVersionLess(v, "0.8.0");
    }
    if (info && info->route == RetroRoute::Tico && !info->tico_module.empty()) {
        r.module_pkg = RetroEmulatorManager::ticoModulePackageId(info->tico_module);
        r.module_installed = emu.isInstalled(r.module_pkg);
    }
    r.system_files = systemFilesFor(console_id);
    return r;
}

std::vector<SystemFileCheck> checkAllSystemFiles() {
    std::vector<SystemFileCheck> all;
    for (const auto& c : RetroCatalogManager::instance().consoles()) {
        auto files = systemFilesFor(c.id);
        all.insert(all.end(), files.begin(), files.end());
    }
    return all;
}

bool ensureRunnerInstalled(const std::string& console_id,
                           const RunnerProgressFn& progress,
                           const std::shared_ptr<std::atomic<bool>>& cancel,
                           std::string& out_error) {
    // Two downloads for the same console must not install the same module at once.
    static std::mutex installMutex;
    std::lock_guard<std::mutex> lock(installMutex);

    RetroReadiness r = checkRetroReadiness(console_id);
    std::vector<std::string> needed;
    if (!r.runner_installed || r.runner_outdated) needed.push_back(r.runner_pkg);
    if (!r.module_pkg.empty() && !r.module_installed) needed.push_back(r.module_pkg);

    auto& emu = RetroEmulatorManager::instance();
    for (const auto& id : needed) {
        if (cancel && cancel->load()) {
            out_error = "cancelled";
            return false;
        }
        EmulatorPackage pkg;
        if (!emu.getPackageCopy(id, pkg)) {
            out_error = "unknown package " + id;
            return false;
        }
        util::logLine("retro: installing " + pkg.name + " for " + console_id);
        bool ok = installEmulatorPackage(pkg,
            [&](const InstallProgress& p) {
                if (progress) progress(pkg.name, p);
            },
            cancel, out_error);
        if (!ok) {
            util::logLine("retro: could not install " + pkg.name + ": " + out_error);
            return false;
        }
    }
    return true;
}

} // namespace catalog
