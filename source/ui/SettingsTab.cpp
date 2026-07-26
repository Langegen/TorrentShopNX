#include "SettingsTab.hpp"
#include "DownloadUiManager.hpp"
#include "UpdatesView.hpp"
#include "../config/config.h"
#include "../net/http_client.h"
#include "../utils/log.h"
#include <borealis/extern/nlohmann/json.hpp>
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
    statusLabel->setText("Скачивание обновления v" + version + "...\nПрогресс: 0%");
    content->addView(statusLabel);
    
    brls::Dialog* progressDialog = new brls::Dialog(content);
    progressDialog->setCancelable(false);
    
    auto progressObj = std::make_shared<AppDownloadProgress>();
    
    brls::RepeatingTimer* timer = new brls::RepeatingTimer();
    timer->setPeriod(200);
    timer->setCallback([progressObj, statusLabel, timer, version]() {
        if (progressObj->aborted.load()) {
            timer->stop();
            delete timer;
            return;
        }
        
        std::string text = "Скачивание обновления v" + version + "...\n";
        uint64_t dl = progressObj->downloaded.load();
        uint64_t tot = progressObj->total.load();
        if (tot > 0) {
            double percent = (double)dl / (double)tot * 100.0;
            char pctBuf[32];
            std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", percent);
            text += "Прогресс: " + std::string(pctBuf) + " (" + formatBytes(dl) + " / " + formatBytes(tot) + ")";
        } else {
            text += "Скачано: " + formatBytes(dl);
        }
        statusLabel->setText(text);
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
        
        progressObj->aborted.store(true);
        
        brls::sync([res, http_code, tmpPath, progressDialog]() {
            progressDialog->close([res, http_code, tmpPath]() {
                struct stat st;
                bool validDownloadedFile = (stat(tmpPath.c_str(), &st) == 0 && st.st_size > 100 * 1024);
                
                util::logLine("downloadAndInstallAppUpdate: res=" + std::to_string(res) + " http=" + std::to_string(http_code) + " size=" + (validDownloadedFile ? std::to_string(st.st_size) : "invalid"));
                
                if (res == CURLE_OK && http_code == 200 && validDownloadedFile) {
                    try {
                        std::string updatePath = g_nroPath + ".update";
                        
                        bool updateSaved = copyFileOverwrite(tmpPath, updatePath);
                        std::remove(tmpPath.c_str());
                        
                        if (updateSaved) {
                            brls::Dialog* pendingDialog = new brls::Dialog("Обновление скачано!\nПерезапустить приложение для завершения установки?");
                            pendingDialog->addButton("Перезапустить", []() {
#ifdef __SWITCH__
                                if (envHasNextLoad()) {
                                    envSetNextLoad(g_nroPath.c_str(), g_nroPath.c_str());
                                }
#endif
                                brls::Application::quit();
                            });
                            pendingDialog->addButton("Позже", [pendingDialog]() { pendingDialog->close(); });
                            pendingDialog->open();
                        } else {
                            brls::Dialog* errDialog = new brls::Dialog("Ошибка сохранения файла обновления.");
                            errDialog->addButton("ОК", [errDialog]() { errDialog->close(); });
                            errDialog->open();
                        }
                    } catch (const std::exception& e) {
                        brls::Dialog* errDialog = new brls::Dialog("Исключение при замене файла приложения:\n" + std::string(e.what()));
                        errDialog->addButton("ОК", [errDialog]() { errDialog->close(); });
                        errDialog->open();
                    }
                } else {
                    std::error_code ec;
                    std::filesystem::remove(tmpPath, ec);
                    
                    std::string errMsg = "Не удалось скачать файл обновления.\n";
                    if (res != CURLE_OK) {
                        errMsg += "cURL ошибка: " + std::to_string(res);
                    } else if (http_code != 200) {
                        errMsg += "HTTP статус: " + std::to_string(http_code);
                    } else if (!validDownloadedFile) {
                        errMsg += "Скачанный файл недействителен или повреждён (" + (stat(tmpPath.c_str(), &st) == 0 ? std::to_string(st.st_size) : "0") + " байт).";
                    }
                    brls::Dialog* errDialog = new brls::Dialog(errMsg);
                    errDialog->addButton("ОК", [errDialog]() { errDialog->close(); });
                    errDialog->open();
                }
            });
        });
    });
}

SettingsTab::SettingsTab() {
}

void SettingsTab::onContentAvailable() {

    auto& cfg = config::ConfigManager::instance();
    auto& dm = ui::DownloadManager::instance().getImpl();

    // 0. Updates manager
    updatesCell->setText("Менеджер обновлений");
    updatesCell->setDetailText("Проверить наличие и установить обновления");
    updatesCell->registerClickAction([](brls::View* view) {
        brls::Application::pushActivity(new ui::UpdatesView());
        return true;
    });

    // 0.2 App Update
    appUpdateCell->setText("Обновление приложения");
    appUpdateCell->setDetailText("Проверить наличие новой версии TorrentShopNX");
    appUpdateCell->registerClickAction([this, &cfg](brls::View* view) {
        brls::Box* content = new brls::Box();
        content->setAxis(brls::Axis::COLUMN);
        content->setPadding(20);
        content->setAlignItems(brls::AlignItems::CENTER);
        
        brls::Label* statusLabel = new brls::Label();
        statusLabel->setFontSize(16);
        statusLabel->setText("Проверка обновлений...");
        content->addView(statusLabel);
        
        brls::Dialog* checkDialog = new brls::Dialog(content);
        checkDialog->setCancelable(false);
        checkDialog->open();
        
        brls::async([checkDialog, &cfg]() {
            net::HttpClient http;
            auto res = http.httpGet(cfg.getEffectiveAppUpdateUrl());
            
            brls::sync([res, checkDialog]() {
                checkDialog->close([res]() {
                    if (res.status_code != 200 || res.body.empty()) {
                        brls::Dialog* dialog = new brls::Dialog("Не удалось проверить обновления.\nПроверьте подключение к сети.");
                        dialog->addButton("ОК", [dialog]() { dialog->close(); });
                        dialog->open();
                        return;
                    }
                    
                    try {
                        auto j = nlohmann::json::parse(res.body);
                        std::string version;
                        std::string url;
                        std::string changelog;
                        
                        if (j.contains("tag_name")) {
                            // GitHub Releases API format
                            version = j.value("tag_name", "");
                            changelog = j.value("body", "");
                            if (j.contains("assets") && j["assets"].is_array()) {
                                for (const auto& asset : j["assets"]) {
                                    std::string assetName = asset.value("name", "");
                                    if (assetName.size() >= 4 && assetName.rfind(".nro") == assetName.size() - 4) {
                                        url = asset.value("browser_download_url", "");
                                        break;
                                    }
                                }
                            }
                        } else {
                            // Custom update.json format
                            version = j.value("version", "");
                            changelog = j.value("changelog", "");
                            url = j.value("url", "");
                        }
                        
                        std::string currentVersion = "2.1"; // Current app version
                        
                        std::string cleanVersion = version;
                        if (!cleanVersion.empty() && (cleanVersion[0] == 'v' || cleanVersion[0] == 'V')) {
                            cleanVersion = cleanVersion.substr(1);
                        }
                        std::string cleanCurrent = currentVersion;
                        if (!cleanCurrent.empty() && (cleanCurrent[0] == 'v' || cleanCurrent[0] == 'V')) {
                            cleanCurrent = cleanCurrent.substr(1);
                        }
                        
                        if (version.empty() || url.empty()) {
                            brls::Dialog* dialog = new brls::Dialog("Неверный формат ответа обновлений.");
                            dialog->addButton("ОК", [dialog]() { dialog->close(); });
                            dialog->open();
                            return;
                        }
                        
                        if (cleanVersion == cleanCurrent) {
                            brls::Dialog* dialog = new brls::Dialog("У вас установлена последняя версия приложения (" + version + ").");
                            dialog->addButton("ОК", [dialog]() { dialog->close(); });
                            dialog->open();
                            return;
                        }
                        
                        std::string msg = "Доступна новая версия: " + version + "\n\nХотите скачать и установить обновление?";
                        
                        brls::Dialog* dialog = new brls::Dialog(msg);
                        dialog->addButton("Да", [url, version, dialog]() {
                            dialog->close([url, version]() {
                                downloadAndInstallAppUpdate(url, version);
                            });
                        });
                        dialog->addButton("Нет", [dialog]() { dialog->close(); });
                        dialog->open();
                        
                    } catch (const std::exception& e) {
                        brls::Dialog* dialog = new brls::Dialog("Ошибка парсинга обновления:\n" + std::string(e.what()));
                        dialog->addButton("ОК", [dialog]() { dialog->close(); });
                        dialog->open();
                    }
                });
            });
        });
        return true;
    });

    // 0.3 Auto App Update
    autoAppUpdateCell->init("Авто-проверка обновлений", cfg.getAutoAppUpdate(), [&cfg](bool value) {
        cfg.setAutoAppUpdate(value);
        cfg.save();
    });

    // 1. Prevent sleep
    keepAwakeCell->init("Предотвращать сон при скачивании", cfg.getKeepAwakeDuringDownloads(), [&cfg](bool value) {
        cfg.setKeepAwakeDuringDownloads(value);
        cfg.save();
    });

    // 2. Download mode
    std::vector<std::string> modes = { "Удаленный TorrServer", "Локальный клиент (libtorrent)" };
    int initialMode = (cfg.getDataMode() == "local_client") ? 1 : 0;
    
    modeCell->init("Режим работы", modes, initialMode, [](int selected) {}, [&cfg, &dm](int selected) {
        if (selected == 1) {
            cfg.setDataMode("local_client");
            dm.dataSourceManager().setMode(datasource::DataSourceMode::LocalClient);
        } else {
            cfg.setDataMode("torrserver");
            dm.dataSourceManager().setMode(datasource::DataSourceMode::Remote);
        }
        cfg.save();
        brls::Application::notify("Режим загрузки изменен");
    });

    // 3. Remote TorrServer URL
    remoteUrlCell->setText("Адрес TorrServer");
    auto updateRemoteUrlDisplay = [this, &cfg]() {
        std::string url = cfg.getTorrServerUrl();
        if (url.length() > 35) {
            url = url.substr(0, 32) + "...";
        } else if (url.empty()) {
            url = "http://127.0.0.1:8090";
        }
        remoteUrlCell->setDetailText(url);
    };
    updateRemoteUrlDisplay();
    remoteUrlCell->registerClickAction([this, updateRemoteUrlDisplay, &cfg, &dm](brls::View* view) {
        brls::Application::getImeManager()->openForText(
            [updateRemoteUrlDisplay, &cfg, &dm](std::string text) {
                if (!text.empty()) {
                    cfg.setTorrServerUrl(text);
                    dm.dataSourceManager().setRemoteUrl(text);
                    cfg.save();
                    brls::Application::notify("Адрес TorrServer обновлен");
                    updateRemoteUrlDisplay();
                }
            },
            "Адрес TorrServer",
            "Введите адрес удаленного TorrServer",
            255,
            cfg.getTorrServerUrl(),
            0
        );
        return true;
    });

    // 4. Catalog JSON URL
    catalogUrlCell->setText("URL каталога JSON");
    auto updateCatalogUrlDisplay = [this, &cfg]() {
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
    catalogUrlCell->registerClickAction([this, updateCatalogUrlDisplay, &cfg](brls::View* view) {
        brls::Application::getImeManager()->openForText(
            [updateCatalogUrlDisplay, &cfg](std::string text) {
                if (!text.empty()) {
                    cfg.setCatalogSourceUrl(text);
                    cfg.save();
                    brls::Application::notify("Адрес каталога обновлен");
                    updateCatalogUrlDisplay();
                }
            },
            "URL каталога JSON",
            "Введите адрес JSON файла каталога игр",
            255,
            cfg.getCatalogSourceUrl(),
            0
        );
        return true;
    });
}

void SettingsTab::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);
    if (resetState) {
        brls::Application::giveFocus(this->updatesCell);
    }
}

void SettingsTab::willDisappear(bool resetState) {
    brls::Activity::willDisappear(resetState);
    brls::Application::giveFocus(nullptr);
}

} // namespace ui
