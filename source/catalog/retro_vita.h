#pragma once

// Installs decrypted PS Vita games straight into Vita3K-nx, the way its own
// installer does for them: the app folder goes to
// sdmc:/switch/vita3k/vita/ux0/app/<TITLE_ID>/ and Vita3K lists it on start.
// Encrypted dumps (NoNpDrm with work.bin, sce_pfs), .pkg files, updates and
// DLC need Vita3K's installer (it decrypts them), so they stay in
// switch/vita3k/install.

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace catalog {
namespace retro {

struct SfoInfo {
    std::string title_id;
    std::string title;
    std::string category;   // "gd" game, "gp" update, "ac" add-on ...
    std::string content_id;
};

// Reads TITLE_ID, TITLE, CATEGORY and CONTENT_ID from param.sfo data.
bool parseParamSfo(const std::string& data, SfoInfo& out);

struct VitaInstallSummary {
    int installed = 0;
    int staged = 0;
    std::vector<std::string> installed_dirs;            // ux0/app/<TITLE_ID>
    std::vector<std::filesystem::path> left_paths;      // what stays in switch/vita3k/install
};

// candidates: .vpk / .zip files and folders saved for one release.
VitaInstallSummary installIntoVita3K(const std::vector<std::filesystem::path>& candidates,
                                     const std::shared_ptr<std::atomic<bool>>& cancel);

} // namespace retro
} // namespace catalog
