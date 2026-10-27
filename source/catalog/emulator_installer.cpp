#include "emulator_installer.h"

#include "../net/http_client.h"
#include "../utils/app_paths.h"
#include "../utils/archive_utils.h"
#include "../utils/file_ops.h"
#include "../utils/log.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#ifndef _WIN32
#include <strings.h>
#endif

namespace fs = std::filesystem;

namespace catalog {

namespace {

bool sameNameIgnoreCase(const std::string& a, const std::string& b) {
#ifdef _WIN32
    return _stricmp(a.c_str(), b.c_str()) == 0;
#else
    return strcasecmp(a.c_str(), b.c_str()) == 0;
#endif
}

bool cancelled(const std::shared_ptr<std::atomic<bool>>& cancel) {
    return cancel && cancel->load();
}

// Downloads url to dest through a temp file in cache/tmp.
bool downloadFile(const std::string& url, const std::string& dest, int64_t expected_size,
                  const std::shared_ptr<std::atomic<bool>>& cancel,
                  const std::function<void(int64_t done, int64_t total)>& on_progress) {
    std::string tmp = std::string(TSNX_CACHE_TMP) + "/" + fs::path(dest).filename().string() + ".part";
    tsnx_ensure_parent_dirs(tmp.c_str());
    std::error_code ec;
    fs::remove(tmp, ec);

    net::HttpClient client;
    client.setTimeout(600);
    client.setCancelFlag(cancel.get());
    client.setProgressCallback([&](int64_t dltotal, int64_t dlnow) {
        if (on_progress) on_progress(dlnow, dltotal > 0 ? dltotal : expected_size);
    });

    bool ok = client.downloadToFile(url, tmp, cancel.get(), 600);
    if (!ok || cancelled(cancel)) {
        fs::remove(tmp, ec);
        return false;
    }

    tsnx_ensure_parent_dirs(dest.c_str());
    fs::remove(dest, ec);
    ec.clear();
    fs::rename(tmp, dest, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(tmp, dest, fs::copy_options::overwrite_existing, ec);
        std::error_code ec2;
        fs::remove(tmp, ec2);
        if (ec) return false;
    }
    return true;
}

// Emulator archives often wrap the .nro in extra folders: bring the folder
// that holds the main executable up to destDir and give the .nro its expected name.
void normalizeExtractedNro(const EmulatorPackage& pkg, const std::string& destDir) {
    std::string targetFile = RetroEmulatorManager::resolvePlatformPath(pkg.install_path);
    fs::path destP(destDir);
    std::string targetName = fs::path(targetFile).filename().string();
    std::error_code ec;

    fs::path found;
    for (const auto& e : fs::recursive_directory_iterator(destDir, ec)) {
        if (e.is_regular_file() && sameNameIgnoreCase(e.path().filename().string(), targetName)) {
            found = e.path();
            break;
        }
    }
    if (found.empty()) {
        for (const auto& e : fs::recursive_directory_iterator(destDir, ec)) {
            if (e.is_regular_file() && e.path().extension() == ".nro") {
                found = e.path();
                break;
            }
        }
    }

    if (!found.empty() && found.parent_path() != destP) {
        fs::path nroDir = found.parent_path();
        std::string moveErr;
        if (!util::movePath(nroDir.string(), destDir, moveErr)) {
            util::logLine("EmulatorInstall: warning moving nested folder: " + moveErr);
        }
        fs::path top = nroDir;
        while (top.has_parent_path() && top.parent_path() != destP && top.parent_path() != top) {
            top = top.parent_path();
        }
        if (top != destP && fs::exists(top, ec)) fs::remove_all(top, ec);
    }

    if (!fs::exists(targetFile, ec)) {
        for (const auto& e : fs::directory_iterator(destDir, ec)) {
            if (!e.is_regular_file()) continue;
            std::string fn = e.path().filename().string();
            if (sameNameIgnoreCase(fn, targetName) || e.path().extension() == ".nro") {
                std::error_code ecRen;
                fs::rename(e.path(), targetFile, ecRen);
                util::logLine("EmulatorInstall: renamed " + e.path().string() + " -> " + targetFile);
                break;
            }
        }
    }
}

} // namespace

bool installEmulatorPackage(const EmulatorPackage& pkg,
                            const InstallProgressFn& progress,
                            const std::shared_ptr<std::atomic<bool>>& cancel,
                            std::string& out_error) {
    auto report = [&](const InstallProgress& p) {
        if (progress) progress(p);
    };
    std::error_code ec;
    util::logLine("EmulatorInstall: installing " + pkg.id + " (" + pkg.version + ")");

    // Several files (BIOS sets and the like).
    if (!pkg.companion_downloads.empty()) {
        int count = static_cast<int>(pkg.companion_downloads.size());
        for (int i = 0; i < count; ++i) {
            const auto& comp = pkg.companion_downloads[i];
            std::string target = RetroEmulatorManager::resolvePlatformPath(comp.install_path);
            std::string name = fs::path(target).filename().string();

            // Optimization: if this identical URL was already downloaded in an earlier companion file, copy it locally
            bool copied = false;
            for (int prev = 0; prev < i; ++prev) {
                if (pkg.companion_downloads[prev].download_url == comp.download_url) {
                    std::string prevTarget = RetroEmulatorManager::resolvePlatformPath(pkg.companion_downloads[prev].install_path);
                    if (fs::is_regular_file(prevTarget, ec) && fs::file_size(prevTarget, ec) > 0) {
                        tsnx_ensure_parent_dirs(target.c_str());
                        fs::remove(target, ec);
                        ec.clear();
                        fs::copy_file(prevTarget, target, fs::copy_options::overwrite_existing, ec);
                        if (!ec) {
                            copied = true;
                            break;
                        }
                    }
                }
            }

            if (copied) {
                InstallProgress p;
                p.stage = InstallProgress::Stage::Downloading;
                p.file = name;
                p.file_index = i;
                p.file_count = count;
                p.done = comp.file_size;
                p.total = comp.file_size;
                p.fraction = static_cast<float>(i + 1) / count;
                report(p);
                continue;
            }

            bool ok = downloadFile(comp.download_url, target, comp.file_size, cancel,
                [&](int64_t done, int64_t total) {
                    InstallProgress p;
                    p.stage = InstallProgress::Stage::Downloading;
                    p.file = name;
                    p.file_index = i;
                    p.file_count = count;
                    p.done = done;
                    p.total = total;
                    float f = total > 0 ? std::min(1.0f, static_cast<float>(done) / total) : 0.0f;
                    p.fraction = (i + f) / count;
                    report(p);
                });
            if (!ok) {
                out_error = cancelled(cancel) ? "cancelled" : ("download failed: " + name);
                return false;
            }
        }
        RetroEmulatorManager::instance().recordInstalledVersion(pkg.id, pkg.version);
        return true;
    }

    std::string fileName = pkg.filename.empty() ? fs::path(pkg.download_url).filename().string() : pkg.filename;
    std::string tmpFile = std::string(TSNX_CACHE_TMP) + "/" + fileName;
    bool dlOk = downloadFile(pkg.download_url, tmpFile, pkg.file_size, cancel,
        [&](int64_t done, int64_t total) {
            InstallProgress p;
            p.stage = InstallProgress::Stage::Downloading;
            p.file = fileName;
            p.done = done;
            p.total = total;
            p.fraction = total > 0 ? std::min(1.0f, static_cast<float>(done) / total) : 0.0f;
            report(p);
        });
    if (!dlOk) {
        out_error = cancelled(cancel) ? "cancelled" : "download failed";
        return false;
    }

    if (pkg.is_archive) {
        std::string destDir = RetroEmulatorManager::resolvePlatformPath(
            pkg.extract_dir.empty() ? "sdmc:/switch" : pkg.extract_dir);
        tsnx_ensure_dir(destDir.c_str());

        // A Tico module is replaced as a whole folder (its settings, saves and
        // BIOS live elsewhere under sdmc:/tico).
        bool isTicoModule = pkg.category == "tico_module";
        if (isTicoModule) {
            fs::path moduleDir = fs::path(RetroEmulatorManager::resolvePlatformPath(pkg.install_path)).parent_path();
            if (fs::exists(moduleDir, ec)) fs::remove_all(moduleDir, ec);
        }

        std::string extractErr;
        bool ok = util::extractArchive(tmpFile, destDir,
            [&](const util::ArchiveProgress& ap) {
                InstallProgress p;
                p.stage = InstallProgress::Stage::Extracting;
                p.file = ap.currentFileName;
                p.done = static_cast<int64_t>(ap.percentage);
                p.total = 100;
                p.fraction = std::min(1.0f, ap.percentage / 100.0f);
                report(p);
            },
            cancel, extractErr);
        fs::remove(tmpFile, ec);
        if (!ok || cancelled(cancel)) {
            out_error = cancelled(cancel) ? "cancelled" : ("extract failed: " + extractErr);
            return false;
        }

        if (isTicoModule) {
            std::string marker = RetroEmulatorManager::resolvePlatformPath(pkg.install_path);
            if (!fs::exists(marker, ec)) {
                out_error = "module.json not found after unpacking";
                return false;
            }
        } else {
            normalizeExtractedNro(pkg, destDir);
            RetroEmulatorManager::instance().healInstalledEmulators();
        }
    } else {
        InstallProgress p;
        p.stage = InstallProgress::Stage::Finishing;
        p.fraction = 1.0f;
        report(p);
        std::string target = RetroEmulatorManager::resolvePlatformPath(pkg.install_path);
        tsnx_ensure_parent_dirs(target.c_str());
        fs::remove(target, ec);
        ec.clear();
        fs::rename(tmpFile, target, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(tmpFile, target, fs::copy_options::overwrite_existing, ec);
            std::error_code ec2;
            fs::remove(tmpFile, ec2);
            if (ec) {
                out_error = "cannot write " + target;
                return false;
            }
        }
    }

    RetroEmulatorManager::instance().recordInstalledVersion(pkg.id, pkg.version);
    util::logLine("EmulatorInstall: installed " + pkg.id + " (" + pkg.version + ")");
    return true;
}

} // namespace catalog
