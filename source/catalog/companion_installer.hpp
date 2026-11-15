#pragma once

#include "homebrew_installer.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace catalog {

// Installs a companion homebrew app (e.g. ModCD or ModKitNX).
// 1. Checks if the package exists in homebrew::Catalog by repo ("owner/repo")
//    and if found, installs via homebrew::Installer.
// 2. If not found or installer fails, queries GitHub Releases API
//    or falls back to direct release download, writing the .nro into
//    destRelativePath (e.g. "switch/ModCD/ModCD.nro").
bool installCompanionApp(const std::string& repo,
                         const std::string& appName,
                         const std::string& defaultFileName,
                         const std::string& destRelativePath,
                         const std::function<void(const homebrew::InstallProgress&)>& onProgress,
                         const std::shared_ptr<std::atomic<bool>>& cancel,
                         std::string& err);

} // namespace catalog
