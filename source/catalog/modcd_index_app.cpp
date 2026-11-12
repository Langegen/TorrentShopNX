// The app's ModCD games index: real HttpClient and SD paths. Kept apart from
// modcd_index.cpp so the tests build without the network code.

#include "modcd_index.hpp"

#include "collections_manager.h"
#include "homebrew_fs.hpp"
#include "utils/app_paths.h"

#include <ctime>

namespace modcd {

Index& Index::instance() {
    static Index index(TSNX_CACHE_HOMEBREW, std::string(catalog::kCollectionsBaseUrl) + "modcd.json",
                       homebrew::appHttpFetch, [] { return static_cast<long long>(std::time(nullptr)); });
    return index;
}

bool isModCdInstalled() {
    auto& cat = homebrew::Catalog::instance();
    if (cat.installed("modcd").present) return true;
    const std::string root = TSNX_SD_ROOT;
    return homebrew::fsx::fileExists(root + "switch/ModCD/ModCD.nro") ||
           homebrew::fsx::fileExists(root + "switch/modcd/modcd.nro") ||
           homebrew::fsx::fileExists(root + "switch/ModCD.nro") ||
           homebrew::fsx::fileExists(root + "switch/modcd.nro");
}

bool isModKitInstalled() {
    auto& cat = homebrew::Catalog::instance();
    if (cat.installed("modkitnx").present || cat.installed("modkit").present) return true;
    const std::string root = TSNX_SD_ROOT;
    return homebrew::fsx::fileExists(root + "switch/ModKitNX/ModKitNX.nro") ||
           homebrew::fsx::fileExists(root + "switch/modkitnx/modkitnx.nro") ||
           homebrew::fsx::fileExists(root + "switch/ModKitNX.nro") ||
           homebrew::fsx::fileExists(root + "switch/modkitnx.nro");
}

}  // namespace modcd
