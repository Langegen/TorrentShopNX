#include "SettingsTab.hpp"
#include "DownloadUiManager.hpp"
#include "StorageTabView.hpp"
#include "../config/config.h"
#include "../utils/log.h"
#include <fstream>
#include <filesystem>
#include <curl/curl.h>
#include <atomic>
#ifdef __SWITCH__
#include <switch.h>
#endif

extern std::string g_nroPath;

namespace ui {

static std::string formatBytes(unsigned long long bytes) {
    double size = static_cast<double>(bytes);
    int unit = 0;
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    while (size >= 1024.0 && unit < 4) { size /= 1024.0; ++unit; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit]);
    return std::string(buf);
}

struct AppDownloadProgress {
    std::atomic<uint64_t> downloaded{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> aborted{false};
    std::ofstream* file = nullptr;
};

static size_t curlWriteCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* prog = static_cast<AppDownloadProgress*>(userdata);
    size_t bytes = size * nmemb;
    if (prog->file && prog->file->is_open()) {
        prog->file->write(static_cast<const char*>(ptr), bytes);
    }
    prog->downloaded += bytes;
    return bytes;
}

static int curlProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    auto* prog = static_cast<AppDownloadProgress*>(clientp);
    if (prog->aborted.load()) {
        return -1; // abort the transfer
    }
    prog->total = dltotal;
    prog->downloaded = dlnow;
    return 0;
}

static bool copyFileOverwrite(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        util::logLine("copyFileOverwrite: failed to open src " + src);
        return false;
    }
    
    std::streamsize fileSize = in.tellg();
    in.seekg(0, std::ios::beg);
    
    if (fileSize < 100 * 1024) {
        util::logLine("copyFileOverwrite: src file " + src + " size too small (" + std::to_string(fileSize) + " bytes)");
        return false;
    }
    
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        util::logLine("copyFileOverwrite: failed to open dst " + dst);
        return false;
    }
    
    char buffer[65536];
    std::streamsize copied = 0;
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
        std::streamsize bytes = in.gcount();
        if (bytes > 0) {
            out.write(buffer, bytes);
            if (out.fail()) {
                util::logLine("copyFileOverwrite: write failed on dst " + dst);
                out.close();
                in.close();
                return false;
            }
            copied += bytes;
        }
    }
    
    out.flush();
    out.close();
    in.close();
    
    util::logLine("copyFileOverwrite: successfully copied " + std::to_string(copied) + " bytes from " + src + " to " + dst);
    return (copied >= 100 * 1024);
}

[[maybe_unused]] static bool replaceNroFile(const std::string& srcPath, const std::string& dstPath) {
    std::string oldPath = dstPath + ".old";
    std::remove(oldPath.c_str());
    
    int r1 = ::rename(dstPath.c_str(), oldPath.c_str());
    util::logLine("replaceNroFile: rename dst -> old (" + dstPath + " -> " + oldPath + ") res=" + std::to_string(r1));
    
    int r2 = ::rename(srcPath.c_str(), dstPath.c_str());
    util::logLine("replaceNroFile: rename src -> dst (" + srcPath + " -> " + dstPath + ") res=" + std::to_string(r2));
    
    if (r2 == 0) {
        std::remove(oldPath.c_str());
        return true;
    }
    
    bool ok = copyFileOverwrite(srcPath, dstPath);
    if (ok) {
        std::remove(srcPath.c_str());
        std::remove(oldPath.c_str());
    }
    return ok;
}

void downloadAndInstallAppUpdate(const std::string& url, const std::string& version) {
    brls::Box* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(20);
    content->setAlignItems(brls::AlignItems::CENTER);
    
    brls::Label* statusLabel = new brls::Label();
    statusLabel->setFontSize(16);
    statusLabel->setText(brls::getStr("app/settings/downloading_update", version, "0%"));
    content->addView(statusLabel);
    
    brls::Dialog* progressDialog = new brls::Dialog(content);
    progressDialog->setCancelable(false);
    
    auto progressObj = std::make_shared<AppDownloadProgress>();
    
    progressDialog->addButton("app/common/cancel"_i18n, [progressObj]() {
        progressObj->aborted.store(true);
    });
    
    brls::RepeatingTimer* timer = new brls::RepeatingTimer();
    timer->setPeriod(200);
    timer->setCallback([progressObj, statusLabel, timer, version]() {
        if (progressObj->aborted.load()) {
            timer->stop();
            delete timer;
            return;
        }
        
        uint64_t dl = progressObj->downloaded.load();
        uint64_t tot = progressObj->total.load();
        std::string progressInfo;
        if (tot > 0) {
            double percent = (double)dl / (double)tot * 100.0;
            char pctBuf[32];
            std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", percent);
            progressInfo = std::string(pctBuf) + " (" + formatBytes(dl) + " / " + formatBytes(tot) + ")";
        } else {
            progressInfo = formatBytes(dl);
        }
        statusLabel->setText(brls::getStr("app/settings/downloading_update", version, progressInfo));
    });
    timer->start();
    
    progressDialog->open();
    
    std::string tmpPath = g_nroPath + ".tmp";
    
    brls::async([url, tmpPath, progressObj, progressDialog, timer, version]() {
        CURL* curl = curl_easy_init();
        CURLcode res = CURLE_FAILED_INIT;
        long http_code = 0;
        
        if (curl) {
            std::error_code dirEc;
            std::filesystem::create_directories(std::filesystem::path(tmpPath).parent_path(), dirEc);
            
            std::ofstream file(tmpPath, std::ios::binary);
            if (file.is_open()) {
                progressObj->file = &file;
                
                struct curl_slist* headers = nullptr;
                headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
                
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, progressObj.get());
                curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlProgressCallback);
                curl_easy_setopt(curl, CURLOPT_XFERINFODATA, progressObj.get());
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L); // 5 mins
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
                
                res = curl_easy_perform(curl);
                if (res == CURLE_OK) {
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                }
                file.close();
                curl_slist_free_all(headers);
            }
            curl_easy_cleanup(curl);
        }
        
        bool userCancelled = progressObj->aborted.load();
        progressObj->aborted.store(true);
        
        brls::sync([res, http_code, tmpPath, progressDialog, userCancelled]() {
            progressDialog->close([res, http_code, tmpPath, userCancelled]() {
                if (userCancelled) {
                    std::error_code ec;
                    std::filesystem::remove(tmpPath, ec);
                    util::logLine("downloadAndInstallAppUpdate: cancelled by user");
                    return;
                }
                
                struct stat st;
                bool validDownloadedFile = (stat(tmpPath.c_str(), &st) == 0 && st.st_size > 100 * 1024);
                
                util::logLine("downloadAndInstallAppUpdate: res=" + std::to_string(res) + " http=" + std::to_string(http_code) + " size=" + (validDownloadedFile ? std::to_string(st.st_size) : "invalid"));
                
                if (res == CURLE_OK && http_code == 200 && validDownloadedFile) {
                    try {
                        std::string updatePath = g_nroPath + ".update";
                        
                        bool updateSaved = copyFileOverwrite(tmpPath, updatePath);
                        std::remove(tmpPath.c_str());
                        
                        if (updateSaved) {
                            brls::Dialog* pendingDialog = new brls::Dialog("app/settings/update_downloaded_restart"_i18n);
                            pendingDialog->addButton("app/settings/restart_btn"_i18n, []() {
#ifdef __SWITCH__
                                if (envHasNextLoad()) {
                                    envSetNextLoad(g_nroPath.c_str(), g_nroPath.c_str());
                                }
#endif
                                brls::Application::quit();
                            });
                            pendingDialog->addButton("app/settings/later_btn"_i18n, [pendingDialog]() { pendingDialog->close(); });
                            pendingDialog->open();
                        } else {
                            brls::Dialog* errDialog = new brls::Dialog("app/settings/update_save_error"_i18n);
                            errDialog->addButton("app/common/ok"_i18n, [errDialog]() { errDialog->close(); });
                            errDialog->open();
                        }
                    } catch (const std::exception& e) {
                        brls::Dialog* errDialog = new brls::Dialog(brls::getStr("app/settings/update_replace_exception", std::string(e.what())));
                        errDialog->addButton("app/common/ok"_i18n, [errDialog]() { errDialog->close(); });
                        errDialog->open();
                    }
                } else {
                    std::error_code ec;
                    std::filesystem::remove(tmpPath, ec);
                    
                    std::string errMsg = "app/settings/update_download_failed"_i18n;
                    if (res != CURLE_OK) {
                        errMsg += brls::getStr("app/settings/update_curl_error", std::to_string(res));
                    } else if (http_code != 200) {
                        errMsg += brls::getStr("app/settings/update_http_status", std::to_string(http_code));
                    } else if (!validDownloadedFile) {
                        errMsg += brls::getStr("app/settings/update_file_corrupted", (stat(tmpPath.c_str(), &st) == 0 ? std::to_string(st.st_size) : "0"));
                    }
                    brls::Dialog* errDialog = new brls::Dialog(errMsg);
                    errDialog->addButton("app/common/ok"_i18n, [errDialog]() { errDialog->close(); });
                    errDialog->open();
                }
            });
        });
    });
}

namespace {

// Возвращает ScrollingFrame с внутренней колонкой для размещения настроек.
static brls::ScrollingFrame* makeTabBox(brls::Box** out_box) {
    auto* scroll = new brls::ScrollingFrame();
    scroll->setWidth(brls::View::AUTO);
    scroll->setHeight(brls::View::AUTO);

    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setWidth(10000);
    box->setHeight(brls::View::AUTO);
    box->setPaddingTop(30);
    box->setPaddingRight(40);
    box->setPaddingBottom(40);
    box->setPaddingLeft(40);
    box->setAlignItems(brls::AlignItems::STRETCH);

    scroll->setContentView(box);
    if (out_box) *out_box = box;
    return scroll;
}

} // namespace

SettingsTab::SettingsTab() {
}

void SettingsTab::onContentAvailable() {
    tabFrame->addTab("app/settings/cat_general"_i18n, [this]() { return buildGeneralTab(); });
    tabFrame->addTab("app/settings/cat_downloads"_i18n, [this]() { return buildDownloadsTab(); });
    tabFrame->addTab("app/settings/cat_storage"_i18n, [this]() { return buildStorageTab(); });
}

brls::View* SettingsTab::buildGeneralTab() {
    auto& cfg = config::ConfigManager::instance();
    brls::Box* box = nullptr;
    brls::ScrollingFrame* scroll = makeTabBox(&box);

    // Язык интерфейса
    std::vector<std::string> languages = {
        "app/settings/lang_auto"_i18n,
        "app/settings/lang_ru"_i18n,
        "app/settings/lang_en"_i18n
    };
    int initialLang = 0;
    std::string curLang = cfg.getLanguage();
    if (curLang == "ru") initialLang = 1;
    else if (curLang == "en-US" || curLang == "en") initialLang = 2;

    auto* languageCell = new brls::SelectorCell();
    languageCell->init("app/settings/language"_i18n, languages, initialLang, [](int selected) {}, [&cfg](int selected) {
        std::string newLang = "auto";
        if (selected == 1) newLang = "ru";
        else if (selected == 2) newLang = "en-US";

        if (newLang != cfg.getLanguage()) {
            cfg.setLanguage(newLang);
            cfg.setLastCatalogUpdateDate(""); // Force catalog refresh for new language
            cfg.save();

            brls::Dialog* restartDialog = new brls::Dialog("app/settings/lang_changed_restart"_i18n);
            restartDialog->addButton("app/settings/restart_btn"_i18n, []() {
#ifdef __SWITCH__
                if (envHasNextLoad()) {
                    envSetNextLoad(g_nroPath.c_str(), g_nroPath.c_str());
                }
#endif
                brls::Application::quit();
            });
            restartDialog->addButton("app/settings/later_btn"_i18n, [restartDialog]() {
                restartDialog->close();
            });
            restartDialog->open();
        }
    });
    box->addView(languageCell);

    // Авто-проверка обновлений приложения
    auto* autoAppUpdateCell = new brls::BooleanCell();
    autoAppUpdateCell->init("app/settings/auto_app_update"_i18n, cfg.getAutoAppUpdate(), [&cfg](bool value) {
        cfg.setAutoAppUpdate(value);
        cfg.save();
    });
    box->addView(autoAppUpdateCell);

    // Кэширование миниатюр обложек
    auto* cacheThumbnailsCell = new brls::BooleanCell();
    cacheThumbnailsCell->init("app/settings/cache_thumbnails"_i18n, cfg.getCacheCoverThumbnails(), [&cfg](bool value) {
        cfg.setCacheCoverThumbnails(value);
        cfg.save();
    });
    box->addView(cacheThumbnailsCell);

    // URL каталога JSON
    auto* catalogUrlCell = new brls::DetailCell();
    catalogUrlCell->setText("app/settings/catalog_url"_i18n);
    auto updateCatalogUrlDisplay = [catalogUrlCell, &cfg]() {
        std::string url = cfg.getCatalogSourceUrl();
        if (url.empty()) {
            url = cfg.getEffectiveCatalogSourceUrl();
        }
        if (url.length() > 35) {
            url = url.substr(0, 32) + "...";
        }
        catalogUrlCell->setDetailText(url);
    };
    updateCatalogUrlDisplay();
    catalogUrlCell->registerClickAction([updateCatalogUrlDisplay, &cfg](brls::View* view) {
        brls::Application::getImeManager()->openForText(
            [updateCatalogUrlDisplay, &cfg](std::string text) {
                if (!text.empty()) {
                    cfg.setCatalogSourceUrl(text);
                    cfg.setLastCatalogUpdateDate(""); // Force catalog refresh with new URL
                    cfg.save();
                    brls::Application::notify("app/settings/catalog_url_updated"_i18n);
                    updateCatalogUrlDisplay();
                }
            },
            "app/settings/catalog_url_dialog_title"_i18n,
            "app/settings/catalog_url_hint"_i18n,
            255,
            cfg.getCatalogSourceUrl(),
            0
        );
        return true;
    });
    box->addView(catalogUrlCell);

    return scroll;
}

brls::View* SettingsTab::buildDownloadsTab() {
    auto& cfg = config::ConfigManager::instance();
    auto& dm = ui::DownloadManager::instance().getImpl();
    brls::Box* box = nullptr;
    brls::ScrollingFrame* scroll = makeTabBox(&box);

    // Предотвращать сон при скачивании
    auto* keepAwakeCell = new brls::BooleanCell();
    keepAwakeCell->init("app/settings/keep_awake"_i18n, cfg.getKeepAwakeDuringDownloads(), [&cfg](bool value) {
        cfg.setKeepAwakeDuringDownloads(value);
        cfg.save();
    });
    box->addView(keepAwakeCell);

    // Тайм-аут отключения подсветки во время загрузки
    std::vector<std::string> backlightOptions = {
        "app/settings/backlight_manual"_i18n,
        "app/settings/backlight_15s"_i18n,
        "app/settings/backlight_30s"_i18n,
        "app/settings/backlight_60s"_i18n,
        "app/settings/backlight_120s"_i18n
    };
    int currentBacklightTimeout = cfg.getBacklightTimeout();
    int initialBacklightIdx = 0;
    if (currentBacklightTimeout == 15) initialBacklightIdx = 1;
    else if (currentBacklightTimeout == 30) initialBacklightIdx = 2;
    else if (currentBacklightTimeout == 60) initialBacklightIdx = 3;
    else if (currentBacklightTimeout == 120) initialBacklightIdx = 4;

    auto* backlightTimeoutCell = new brls::SelectorCell();
    backlightTimeoutCell->init("app/settings/backlight_timeout"_i18n, backlightOptions, initialBacklightIdx, [](int selected) {}, [&cfg](int selected) {
        int timeoutSec = 0;
        if (selected == 1) timeoutSec = 15;
        else if (selected == 2) timeoutSec = 30;
        else if (selected == 3) timeoutSec = 60;
        else if (selected == 4) timeoutSec = 120;
        cfg.setBacklightTimeout(timeoutSec);
        cfg.save();
    });
    box->addView(backlightTimeoutCell);

    // Режим работы (движок / TorrServer)
    std::vector<std::string> modes = {
        "app/settings/mode_torrserver"_i18n,
        "app/settings/mode_engine"_i18n
    };
    int initialMode = 0;
    if (cfg.getDataMode() == "local_client" || cfg.getDataMode() == "custom_engine") initialMode = 1;

    auto* modeCell = new brls::SelectorCell();
    modeCell->init("app/settings/data_mode"_i18n, modes, initialMode, [](int selected) {}, [&cfg, &dm](int selected) {
        if (selected == 1) {
            cfg.setDataMode("custom_engine");
            dm.dataSourceManager().setMode(datasource::DataSourceMode::CustomEngine);
        } else {
            cfg.setDataMode("torrserver");
            dm.dataSourceManager().setMode(datasource::DataSourceMode::Remote);
        }
        cfg.save();
        brls::Application::notify("app/settings/mode_changed"_i18n);
    });
    box->addView(modeCell);

    // Адрес удалённого TorrServer
    auto* remoteUrlCell = new brls::DetailCell();
    remoteUrlCell->setText("app/settings/remote_url"_i18n);
    auto updateRemoteUrlDisplay = [remoteUrlCell, &cfg]() {
        std::string url = cfg.getTorrServerUrl();
        if (url.length() > 35) {
            url = url.substr(0, 32) + "...";
        } else if (url.empty()) {
            url = "http://127.0.0.1:8090";
        }
        remoteUrlCell->setDetailText(url);
    };
    updateRemoteUrlDisplay();
    remoteUrlCell->registerClickAction([remoteUrlCell, updateRemoteUrlDisplay, &cfg, &dm](brls::View* view) {
        brls::Application::getImeManager()->openForText(
            [updateRemoteUrlDisplay, &cfg, &dm](std::string text) {
                if (!text.empty()) {
                    cfg.setTorrServerUrl(text);
                    dm.dataSourceManager().setRemoteUrl(text);
                    cfg.save();
                    brls::Application::notify("app/settings/remote_url_updated"_i18n);
                    updateRemoteUrlDisplay();
                }
            },
            "app/settings/remote_url_dialog_title"_i18n,
            "app/settings/remote_url_hint"_i18n,
            255,
            cfg.getTorrServerUrl(),
            0
        );
        return true;
    });
    box->addView(remoteUrlCell);

    return scroll;
}

brls::View* SettingsTab::buildStorageTab() {
    return new StorageTabView();
}

void SettingsTab::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    if (resetState) {
        tabFrame->focusTab(0);
    }
}

void SettingsTab::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    brls::Application::giveFocus(nullptr);
}

} // namespace ui