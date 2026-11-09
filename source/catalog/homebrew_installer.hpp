#pragma once

// Installs, updates and removes homebrew store packages the way libget does,
// so the Homebrew App Store and we share sdmc:/switch/appstore/.get/packages.
//
// install():  file list (inline or the package's manifest.install) -> path
//             check -> free space -> download -> SHA-256 / MD5 -> every file
//             extracted next to its target as "<file>.hbnew" -> only when all
//             are there, they replace the targets -> old files gone from the
//             new version are deleted -> registry written. A failure or cancel
//             before the replace step leaves the card as it was.
// uninstall(): files from the registry manifest (never a forbidden path),
//             empty folders, the registry entry.
//
// Network and free space are passed in (pctest/test_homebrew_installer.cpp);
// the app's instance is in homebrew_installer_app.cpp.

#include "homebrew_catalog.hpp"
#include "homebrew_package.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace homebrew {

struct InstallProgress {
    enum class Stage { Prepare, Download, Verify, Extract, Finish, Remove };
    Stage stage = Stage::Prepare;
    std::string file;      // file being written (Extract) or removed
    int64_t done = 0;      // bytes
    int64_t total = 0;
    float fraction = 0.f;  // whole operation, 0..1
};
using InstallProgressFn = std::function<void(const InstallProgress&)>;

// Download url to dest. progress(done, total); total may be 0 if unknown.
using DownloadFn = std::function<bool(const std::string& url, const std::string& dest,
                                      const std::function<void(int64_t, int64_t)>& progress,
                                      const std::atomic<bool>* cancel, std::string& err)>;
// Free bytes on the volume of path; -1 if unknown.
using FreeSpaceFn = std::function<int64_t(const std::string& path)>;

struct InstallerConfig {
    std::string sd_root;       // "sdmc:/"
    std::string registry_dir;  // sd_root + "switch/appstore/.get/packages"
    std::string tmp_dir;       // downloads
};

enum class InstallError {
    None,
    Cancelled,
    Network,      // manifest or package download failed
    Checksum,     // SHA-256 / MD5 mismatch
    NoSpace,
    Forbidden,    // the package writes where it must not (bootloader, ...)
    BadArchive,   // broken zip, unsupported compression
    Write,        // card write failed
    NotInstalled  // uninstall of a package without a registry entry
};

struct InstallResult {
    bool ok = false;
    InstallError error = InstallError::None;
    std::string message;                 // English, for the log
    std::vector<std::string> written;    // files put on the card
    std::vector<std::string> kept;       // settings left as they were (G entries)
    std::vector<std::string> removed;    // files deleted (stale ones, or on uninstall)
    std::vector<std::string> missing;    // listed in the manifest but not in the archive (skipped, like libget)
};

class Installer {
public:
    Installer(InstallerConfig cfg, Fetcher fetch, DownloadFn download, FreeSpaceFn free_space);

    static Installer& instance();  // homebrew_installer_app.cpp

    // current: what the registry says now (Catalog::installed(pkg.name)).
    InstallResult install(const Package& pkg, const Installed& current, const InstallProgressFn& progress,
                          const std::shared_ptr<std::atomic<bool>>& cancel);

    // keep_settings: G entries and config/ stay on the card.
    InstallResult uninstall(const std::string& name, bool keep_settings, const InstallProgressFn& progress);

    const InstallerConfig& config() const { return cfg_; }

private:
    std::string registryDirFor(const std::string& name) const;  // existing folder, any case

    InstallerConfig cfg_;
    Fetcher fetch_;
    DownloadFn download_;
    FreeSpaceFn free_space_;
    std::mutex mutex_;  // one operation at a time
};

// Packages pkg needs that are not installed yet, dependencies first.
// Unknown names and cycles are skipped.
std::vector<Package> missingDependencies(const Package& pkg, const Catalog& catalog);

const char* installErrorName(InstallError e);

}  // namespace homebrew
