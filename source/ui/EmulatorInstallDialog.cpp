#include "EmulatorInstallDialog.hpp"
#include "../catalog/retro_emulator_manager.h"
#include "../net/http_client.h"
#include "../utils/archive_utils.h"
#include "../utils/app_paths.h"
#include "../utils/file_ops.h"
#include "../utils/log.h"
#include <borealis.hpp>
#include <filesystem>
#include <chrono>
#include <atomic>
#include <memory>
#include <iomanip>
#include <sstream>

namespace ui {

namespace {

std::string formatSizeMb(int64_t bytes) {
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << mb << " MB";
    return ss.str();
}

} // namespace

void showEmulatorInstallDialog(const catalog::EmulatorPackage& pkg,
                               std::function<void(bool success)> onComplete) {
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidth(480.0f);
    content->setPadding(20.0f);
    content->setAlignItems(brls::AlignItems::STRETCH);

    auto* titleLabel = new brls::Label();
    titleLabel->setText("app/retro/installing_title"_i18n + pkg.name);
    titleLabel->setFontSize(20.0f);
    titleLabel->setTextColor(nvgRGB(255, 255, 255));
    titleLabel->setMarginBottom(8.0f);
    content->addView(titleLabel);

    auto* statusLabel = new brls::Label();
    statusLabel->setText("app/retro/preparing_download"_i18n);
    statusLabel->setFontSize(14.0f);
    statusLabel->setTextColor(nvgRGB(180, 180, 190));
    statusLabel->setMarginBottom(12.0f);
    content->addView(statusLabel);

    auto* progressBg = new brls::Box();
    progressBg->setAxis(brls::Axis::ROW);
    progressBg->setWidth(440.0f);
    progressBg->setHeight(10.0f);
    progressBg->setCornerRadius(5.0f);
    progressBg->setBackgroundColor(nvgRGBA(42, 45, 52, 255));
    progressBg->setMarginBottom(8.0f);

    auto* progressFill = new brls::Box();
    progressFill->setWidth(0.0f);
    progressFill->setHeight(10.0f);
    progressFill->setCornerRadius(5.0f);
    progressFill->setBackgroundColor(pkg.color);
    progressBg->addView(progressFill);
    content->addView(progressBg);

    auto* statsLabel = new brls::Label();
    statsLabel->setText("0.0 MB / " + formatSizeMb(pkg.file_size));
    statsLabel->setFontSize(12.0f);
    statsLabel->setTextColor(nvgRGB(130, 130, 140));
    statsLabel->setMarginBottom(14.0f);
    content->addView(statsLabel);

    auto* dialog = new brls::Dialog(content);
    dialog->setCancelable(false);

    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto closedFlag = std::make_shared<std::atomic<bool>>(false);

    std::string cancelText = brls::getStr("app/common/cancel");
    if (cancelText.empty() || cancelText == "app/common/cancel") cancelText = "Cancel";

    dialog->addButton(cancelText, [cancelFlag, statusLabel]() {
        cancelFlag->store(true);
        statusLabel->setText("app/retro/cancelling_install"_i18n);
    });

    dialog->open();

    brls::async([pkg, cancelFlag, closedFlag, dialog, statusLabel, progressFill, statsLabel, onComplete]() {
        std::string tmpFile = TSNX_CACHE_TMP "/" + pkg.filename;
        tsnx_ensure_parent_dirs(tmpFile.c_str());

        std::error_code ec;
        std::filesystem::remove(tmpFile, ec);

        util::logLine("EmulatorInstall: starting download for " + pkg.name + " from " + pkg.download_url);

        net::HttpClient client;
        client.setTimeout(600);
        client.setCancelFlag(cancelFlag.get());

        auto lastUpdate = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());

        client.setProgressCallback([lastUpdate, statusLabel, progressFill, statsLabel, cancelFlag](int64_t dltotal, int64_t dlnow) {
            if (cancelFlag->load()) return;

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastUpdate).count() < 60 && dlnow < dltotal) {
                return;
            }
            *lastUpdate = now;

            brls::sync([statusLabel, progressFill, statsLabel, dltotal, dlnow]() {
                if (dltotal > 0) {
                    float pct = static_cast<float>(dlnow) / static_cast<float>(dltotal);
                    progressFill->setWidth(440.0f * pct);
                    statusLabel->setText(brls::getStr("app/retro/downloading_files_pct", std::to_string(static_cast<int>(pct * 100))));
                    statsLabel->setText(formatSizeMb(dlnow) + " / " + formatSizeMb(dltotal));
                } else if (dlnow > 0) {
                    statusLabel->setText("app/retro/downloading_files"_i18n);
                    statsLabel->setText(formatSizeMb(dlnow));
                }
            });
        });

        bool dlOk = client.downloadToFile(pkg.download_url, tmpFile, cancelFlag.get(), 600);

        if (cancelFlag->load()) {
            std::filesystem::remove(tmpFile, ec);
            brls::sync([dialog, closedFlag, onComplete]() {
                if (!closedFlag->exchange(true)) {
                    dialog->close([onComplete]() {
                        brls::Application::notify("app/retro/install_cancelled"_i18n);
                        if (onComplete) onComplete(false);
                    });
                }
            });
            return;
        }

        if (!dlOk) {
            std::filesystem::remove(tmpFile, ec);
            util::logLine("EmulatorInstall: failed to download " + pkg.name);
            brls::sync([dialog, closedFlag, onComplete]() {
                if (!closedFlag->exchange(true)) {
                    dialog->close([onComplete]() {
                        brls::Application::notify("app/retro/download_error_offline"_i18n);
                        if (onComplete) onComplete(false);
                    });
                }
            });
            return;
        }

        // --- Phase 2: Installation / Extraction ---
        if (pkg.is_archive) {
            brls::sync([statusLabel, progressFill, statsLabel]() {
                statusLabel->setText("app/retro/extracting_archive"_i18n);
                progressFill->setWidth(0.0f);
                statsLabel->setText("app/retro/extracting_to_sd"_i18n);
            });

            std::string destDir = catalog::RetroEmulatorManager::resolvePlatformPath(
                pkg.extract_dir.empty() ? "sdmc:/switch" : pkg.extract_dir
            );
            tsnx_ensure_dir(destDir.c_str());

            std::string extractErr;
            bool extractOk = util::extractArchive(
                tmpFile,
                destDir,
                [statusLabel, progressFill, statsLabel, lastUpdate, cancelFlag](const util::ArchiveProgress& prog) {
                    if (cancelFlag->load()) return;
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastUpdate).count() < 60 && prog.percentage < 100.0f) {
                        return;
                    }
                    *lastUpdate = now;

                    brls::sync([statusLabel, progressFill, statsLabel, prog]() {
                        progressFill->setWidth(440.0f * (prog.percentage / 100.0f));
                        statusLabel->setText(brls::getStr("app/retro/extracting_pct", std::to_string(static_cast<int>(prog.percentage))));
                        if (!prog.currentFileName.empty()) {
                            statsLabel->setText(prog.currentFileName);
                        }
                    });
                },
                cancelFlag,
                extractErr
            );

            std::filesystem::remove(tmpFile, ec);

            if (!extractOk || cancelFlag->load()) {
                util::logLine("EmulatorInstall: extract failed for " + pkg.name + ": " + extractErr);
                brls::sync([dialog, closedFlag, extractErr, onComplete]() {
                    if (!closedFlag->exchange(true)) {
                        dialog->close([onComplete, extractErr]() {
                            brls::Application::notify("app/retro/extract_error_prefix"_i18n + extractErr);
                            if (onComplete) onComplete(false);
                        });
                    }
                });
                return;
            }

            // Post-extraction: ensure targetFile and companion folders (assets, licenses, etc.) are at destDir root
            std::string targetFile = catalog::RetroEmulatorManager::resolvePlatformPath(pkg.install_path);
            std::filesystem::path targetP(targetFile);
            std::filesystem::path destP(destDir);
            std::string targetFilename = targetP.filename().string();

            std::error_code ecIter;
            std::filesystem::path foundNroPath;

            // 1. Search for matching filename (case-insensitive) in destDir
            for (const auto& entry : std::filesystem::recursive_directory_iterator(destDir, ecIter)) {
                if (entry.is_regular_file()) {
                    std::string fn = entry.path().filename().string();
#ifdef _WIN32
                    if (_stricmp(fn.c_str(), targetFilename.c_str()) == 0) {
#else
                    if (strcasecmp(fn.c_str(), targetFilename.c_str()) == 0) {
#endif
                        foundNroPath = entry.path();
                        break;
                    }
                }
            }

            // 2. Fallback: search for any .nro file in destDir
            if (foundNroPath.empty()) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(destDir, ecIter)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".nro") {
                        foundNroPath = entry.path();
                        break;
                    }
                }
            }

            // 3. If found inside a nested subfolder of destDir, move entire contents of that folder up to destDir
            if (!foundNroPath.empty()) {
                std::filesystem::path nroDir = foundNroPath.parent_path();
                if (nroDir != destP) {
                    std::string moveErr;
                    util::logLine("EmulatorInstall: moving nested directory contents from " + nroDir.string() + " to " + destDir);
                    if (util::movePath(nroDir.string(), destDir, moveErr)) {
                        util::logLine("EmulatorInstall: successfully merged nested folder into " + destDir);
                    } else {
                        util::logLine("EmulatorInstall: warning moving nested folder: " + moveErr);
                    }

                    // Clean up intermediate directories up to destDir
                    std::error_code ecRm;
                    std::filesystem::path curr = nroDir;
                    while (curr.has_parent_path() && curr.parent_path() != destP && curr.parent_path() != curr) {
                        curr = curr.parent_path();
                    }
                    if (curr != destP && std::filesystem::exists(curr, ecRm)) {
                        std::filesystem::remove_all(curr, ecRm);
                    }
                }
            }

            // 4. Ensure targetFile exists with the exact canonical name in destDir
            if (!std::filesystem::exists(targetFile)) {
                std::error_code ecDir;
                // Try case-insensitive match in destDir root
                for (const auto& entry : std::filesystem::directory_iterator(destDir, ecDir)) {
                    if (entry.is_regular_file()) {
                        std::string fn = entry.path().filename().string();
#ifdef _WIN32
                        if (_stricmp(fn.c_str(), targetFilename.c_str()) == 0) {
#else
                        if (strcasecmp(fn.c_str(), targetFilename.c_str()) == 0) {
#endif
                            std::error_code ecRen;
                            std::filesystem::rename(entry.path(), targetFile, ecRen);
                            util::logLine("EmulatorInstall: normalized executable name from " + entry.path().string() + " to " + targetFile);
                            break;
                        }
                    }
                }
            }

            // Fallback: if targetFile still doesn't exist, rename any .nro at destDir root
            if (!std::filesystem::exists(targetFile)) {
                std::error_code ecDir;
                for (const auto& entry : std::filesystem::directory_iterator(destDir, ecDir)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".nro") {
                        std::error_code ecRen;
                        std::filesystem::rename(entry.path(), targetFile, ecRen);
                        util::logLine("EmulatorInstall: fallback renamed .nro to " + targetFile);
                        break;
                    }
                }
            }

            // 5. Run auto-healing for any known packages (assets relocation, etc.)
            catalog::RetroEmulatorManager::instance().healInstalledEmulators();
        } else {
            // Standalone .nro
            brls::sync([statusLabel]() {
                statusLabel->setText("app/retro/moving_files"_i18n);
            });

            std::string targetFile = catalog::RetroEmulatorManager::resolvePlatformPath(pkg.install_path);
            tsnx_ensure_parent_dirs(targetFile.c_str());

            std::filesystem::remove(targetFile, ec);
            std::filesystem::rename(tmpFile, targetFile, ec);
            if (ec) {
                // If rename across filesystems failed, try copy + remove
                std::filesystem::copy_file(tmpFile, targetFile, std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::remove(tmpFile, ec);
            }
        }

        // Record installation
        catalog::RetroEmulatorManager::instance().recordInstalledVersion(pkg.id, pkg.version);
        util::logLine("EmulatorInstall: successfully installed " + pkg.name + " (" + pkg.version + ")");

        brls::sync([dialog, closedFlag, pkg, onComplete]() {
            if (!closedFlag->exchange(true)) {
                dialog->close([pkg, onComplete]() {
                    brls::Application::notify(brls::getStr("app/retro/emu_installed_success_format", pkg.name));
                    if (onComplete) onComplete(true);
                });
            }
        });
    });
}

} // namespace ui
