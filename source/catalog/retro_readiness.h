#pragma once

// Is everything in place to play a console's games: the emulator (Tico,
// Vita3K or Cemu), the Tico module for the console, and the system files
// (BIOS, PS Vita firmware) the emulator asks for. Also installs what can be
// installed automatically.

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "emulator_installer.h"

namespace catalog {

struct SystemFileCheck {
    std::string console_id;
    std::string hint_key;   // i18n key: what file is needed
    std::string dir;        // where it goes, as shown to the user (sdmc:/...)
    bool present = false;
    bool optional = false;  // only some games need it
};

struct RetroReadiness {
    std::string runner_pkg;          // "tico", "vita3k" or "cemu"
    bool runner_installed = false;
    bool runner_outdated = false;    // Tico older than 0.8 has no modules
    std::string module_pkg;          // "tico_module_<id>", empty outside Tico
    bool module_installed = false;
    std::vector<SystemFileCheck> system_files;

    bool emulatorReady() const;
    bool systemFilesReady() const;   // required files only
};

RetroReadiness checkRetroReadiness(const std::string& console_id);

// System files of every console that needs them (for the emulators screen).
std::vector<SystemFileCheck> checkAllSystemFiles();

// Installs or updates whatever emulator package the console still lacks.
// Thread-safe; runs synchronously, call it off the UI thread.
using RunnerProgressFn = std::function<void(const std::string& package_name, const InstallProgress& progress)>;
bool ensureRunnerInstalled(const std::string& console_id,
                           const RunnerProgressFn& progress,
                           const std::shared_ptr<std::atomic<bool>>& cancel,
                           std::string& out_error);

} // namespace catalog
