#include "companion_installer.hpp"

#include "../net/http_client.h"
#include "../utils/app_paths.h"
#include "../utils/file_ops.h"
#include "../utils/log.h"
#include "homebrew_catalog.hpp"

#include <borealis/extern/nlohmann/json.hpp>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace catalog {

bool installCompanionApp(const std::string& repo,
                         const std::string& appName,
                         const std::string& defaultFileName,
                         const std::string& destRelativePath,
                         const std::function<void(const homebrew::InstallProgress&)>& onProgress,
                         const std::shared_ptr<std::atomic<bool>>& cancel,
                         std::string& err) {
    if (cancel && cancel->load()) {
        err = "Cancelled";
        return false;
    }

    if (onProgress) {
        onProgress({homebrew::InstallProgress::Stage::Prepare, defaultFileName, 0, 0, 0.05f});
    }

    // 1. Attempt install via Homebrew Catalog
    homebrew::Package pkg;
    if (homebrew::Catalog::instance().findByRepo(repo, pkg)) {
        util::logLine("companion_installer: found " + appName + " in catalog as " + pkg.name);
        const auto current = homebrew::Catalog::instance().installed(pkg.name);
        const auto res = homebrew::Installer::instance().install(pkg, current, onProgress, cancel);
        if (res.ok) {
            homebrew::Catalog::instance().rescanInstalled();
            util::logLine("companion_installer: successfully installed " + appName + " via catalog");
            return true;
        }
        if (cancel && cancel->load()) {
            err = "Cancelled";
            return false;
        }
        util::logLine("companion_installer: catalog install failed (" + res.message + "), falling back to GitHub");
    }

    if (cancel && cancel->load()) {
        err = "Cancelled";
        return false;
    }

    // 2. Direct download from GitHub Releases
    if (onProgress) {
        onProgress({homebrew::InstallProgress::Stage::Prepare, defaultFileName, 0, 0, 0.1f});
    }

    std::string downloadUrl;
    int64_t expectedSize = 0;

    // Try GitHub API first
    const std::string apiUrl = "https://api.github.com/repos/" + repo + "/releases/latest";
    net::HttpClient apiHttp;
    apiHttp.setTimeout(15);
    apiHttp.setCancelFlag(cancel.get());
    auto resp = apiHttp.httpGet(apiUrl, {"User-Agent: TorrentShopNX", "Accept: application/vnd.github.v3+json"});

    if (resp.status_code == 200 && !resp.body.empty()) {
        try {
            nlohmann::json rel = nlohmann::json::parse(resp.body, nullptr, false);
            if (rel.is_object() && rel.contains("assets") && rel["assets"].is_array()) {
                for (const auto& a : rel["assets"]) {
                    std::string name = a.value("name", "");
                    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".nro") == 0) {
                        downloadUrl = a.value("browser_download_url", "");
                        expectedSize = a.value("size", 0ULL);
                        break;
                    }
                }
            }
        } catch (...) {
            util::logLine("companion_installer: failed to parse GitHub API response for " + repo);
        }
    }

    // If API didn't yield an asset (e.g. rate limit 403 or empty), fallback to predictable direct URL
    if (downloadUrl.empty()) {
        downloadUrl = "https://github.com/" + repo + "/releases/latest/download/" + defaultFileName;
        util::logLine("companion_installer: using direct GitHub download URL: " + downloadUrl);
    } else {
        util::logLine("companion_installer: resolved asset URL from GitHub API: " + downloadUrl);
    }

    if (cancel && cancel->load()) {
        err = "Cancelled";
        return false;
    }

    const std::string tmpDest = std::string(TSNX_CACHE_TMP) + "/" + defaultFileName + ".part";
    tsnx_ensure_parent_dirs(tmpDest.c_str());
    std::remove(tmpDest.c_str());

    net::HttpClient dlClient;
    dlClient.setTimeout(600);
    dlClient.setCancelFlag(cancel.get());
    dlClient.setProgressCallback([&](int64_t dltotal, int64_t dlnow) {
        if (cancel && cancel->load()) return;
        homebrew::InstallProgress p;
        p.stage = homebrew::InstallProgress::Stage::Download;
        p.file = defaultFileName;
        p.done = dlnow;
        p.total = dltotal > 0 ? dltotal : (expectedSize > 0 ? expectedSize : 0);
        float frac = 0.1f + (p.total > 0 ? static_cast<float>(dlnow) / static_cast<float>(p.total) * 0.8f : 0.4f);
        if (frac > 0.9f) frac = 0.9f;
        p.fraction = frac;
        if (onProgress) onProgress(p);
    });

    const bool dlOk = dlClient.downloadToFile(downloadUrl, tmpDest, cancel.get(), 600);
    if (!dlOk || (cancel && cancel->load())) {
        std::remove(tmpDest.c_str());
        err = (cancel && cancel->load()) ? "Cancelled" : "Failed to download " + defaultFileName;
        return false;
    }

    // 3. Move file to destination
    if (onProgress) {
        onProgress({homebrew::InstallProgress::Stage::Finish, defaultFileName, 0, 0, 0.95f});
    }

    const std::string finalDest = std::string(TSNX_SD_ROOT) + destRelativePath;
    tsnx_ensure_parent_dirs(finalDest.c_str());
    std::remove(finalDest.c_str());

    std::error_code ec;
    fs::rename(tmpDest, finalDest, ec);
    if (ec) {
        // Fallback to copy + remove
        fs::copy_file(tmpDest, finalDest, fs::copy_options::overwrite_existing, ec);
        std::remove(tmpDest.c_str());
        if (ec) {
            err = "Failed to copy file to " + finalDest + ": " + ec.message();
            return false;
        }
    }

    // Update catalog installed state
    homebrew::Catalog::instance().rescanInstalled();

    if (onProgress) {
        onProgress({homebrew::InstallProgress::Stage::Finish, defaultFileName, 0, 0, 1.0f});
    }

    util::logLine("companion_installer: successfully installed " + appName + " to " + finalDest);
    return true;
}

} // namespace catalog
