#include "SettingsTab.hpp"
#include "ThemeManager.hpp"
#include "BackgroundFilePicker.hpp"
#include "DownloadUiManager.hpp"
#include "StorageTabView.hpp"
#include "QrCodeView.hpp"
#include "RetroUpdateDialog.hpp"
#include "../catalog/retro_catalog_manager.h"
#include "../catalog/catalog_updater.h"
#include "../GameData.hpp"
#include "../config/config.h"
#include "../utils/log.h"
#include "../utils/switch_utils.h"
#include "../net/http_client.h"
#include <borealis/extern/nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <curl/curl.h>
#include <atomic>
#ifdef __SWITCH__
#include <switch.h>
#include <unistd.h>
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
    
    auto timer = std::make_shared<brls::RepeatingTimer>();
    timer->setPeriod(200);
    timer->setCallback([progressObj, statusLabel, version]() {
        if (progressObj->aborted.load()) {
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
        
        brls::sync([res, http_code, tmpPath, progressDialog, timer, userCancelled]() {
            timer->stop();
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
                        
                        std::remove(updatePath.c_str());
                        int renRes = ::rename(tmpPath.c_str(), updatePath.c_str());
                        bool updateSaved = false;
                        if (renRes == 0) {
                            updateSaved = true;
                            util::logLine("downloadAndInstallAppUpdate: successfully renamed tmp to " + updatePath);
                        } else {
                            util::logLine("downloadAndInstallAppUpdate: rename failed (res=" + std::to_string(renRes) + "), falling back to copyFileOverwrite");
                            updateSaved = copyFileOverwrite(tmpPath, updatePath);
                            std::remove(tmpPath.c_str());
                        }
                        
                        if (updateSaved) {
#ifdef __SWITCH__
                            fsdevCommitDevice("sdmc");
#endif
                            brls::Dialog* pendingDialog = new brls::Dialog("app/settings/update_downloaded_restart"_i18n);
                            pendingDialog->addButton("app/settings/restart_btn"_i18n, []() {
                                brls::Application::getPlatform()->exitToHomeMode(false);
                                brls::Application::quit();
                            });
                            pendingDialog->addButton("app/settings/later_btn"_i18n, []() {});
                            pendingDialog->open();
                        } else {
                            brls::Dialog* errDialog = new brls::Dialog("app/settings/update_save_error"_i18n);
                            errDialog->addButton("app/common/ok"_i18n, []() {});
                            errDialog->open();
                        }
                    } catch (const std::exception& e) {
                        brls::Dialog* errDialog = new brls::Dialog(brls::getStr("app/settings/update_replace_exception", std::string(e.what())));
                        errDialog->addButton("app/common/ok"_i18n, []() {});
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
                    errDialog->addButton("app/common/ok"_i18n, []() {});
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
    // Компактный сайдбар категорий в виде стеклянной рамки дашборда
    brls::getStyle().addMetric("brls/tab_frame/sidebar_width", 260.0f);
    brls::getStyle().addMetric("brls/sidebar/padding_top", 16.0f);
    brls::getStyle().addMetric("brls/sidebar/padding_bottom", 16.0f);
    brls::getStyle().addMetric("brls/sidebar/padding_left", 20.0f);
    brls::getStyle().addMetric("brls/sidebar/padding_right", 20.0f);
    brls::getStyle().addMetric("brls/sidebar/item_height", 56.0f);
    brls::getStyle().addMetric("brls/sidebar/item_font_size", 19.0f);
    brls::getStyle().addMetric("brls/sidebar/item_accent_margin_sides", 10.0f);
}

void SettingsTab::onContentAvailable() {
    brls::View* sidebar = tabFrame->getView("brls/tab_frame/sidebar");
    if (sidebar) {
        sidebar->setBackground(brls::ViewBackground::SIDEBAR);
        sidebar->setMarginRight(24.0f);
        sidebar->setCornerRadius(14.0f);
    }

    tabFrame->addTab("app/settings/cat_general"_i18n, [this]() { return buildGeneralTab(); });
    tabFrame->addTab("app/settings/cat_appearance"_i18n, [this]() { return buildAppearanceTab(); });
    tabFrame->addTab("app/settings/cat_downloads"_i18n, [this]() { return buildDownloadsTab(); });
    tabFrame->addTab("app/settings/cat_retro"_i18n, [this]() { return buildRetroTab(); });
    tabFrame->addTab("app/settings/cat_storage"_i18n, [this]() { return buildStorageTab(); });
    tabFrame->addTab("app/settings/cat_about"_i18n, [this]() { return buildAboutTab(); });
}

brls::View* SettingsTab::buildGeneralTab() {
    auto& cfg = config::ConfigManager::instance();
    brls::Box* box = nullptr;
    brls::ScrollingFrame* scroll = makeTabBox(&box);

    // Язык интерфейса
    std::vector<std::string> languages = {
        "app/settings/lang_auto"_i18n,
        "app/settings/lang_ru"_i18n,
        "app/settings/lang_en"_i18n,
        "app/settings/lang_es"_i18n,
        "app/settings/lang_fr"_i18n,
        "app/settings/lang_de"_i18n,
        "app/settings/lang_it"_i18n,
        "app/settings/lang_pt_br"_i18n,
        "app/settings/lang_zh_hans"_i18n,
        "app/settings/lang_ja"_i18n
    };
    int initialLang = 0;
    std::string curLang = cfg.getLanguage();
    if (curLang == "ru") initialLang = 1;
    else if (curLang == "en-US" || curLang == "en") initialLang = 2;
    else if (curLang == "es") initialLang = 3;
    else if (curLang == "fr") initialLang = 4;
    else if (curLang == "de") initialLang = 5;
    else if (curLang == "it") initialLang = 6;
    else if (curLang == "pt-BR" || curLang == "pt") initialLang = 7;
    else if (curLang == "zh-Hans" || curLang == "zh-CN" || curLang == "zh") initialLang = 8;
    else if (curLang == "ja") initialLang = 9;

    auto* languageCell = new brls::SelectorCell();
    languageCell->init("app/settings/language"_i18n, languages, initialLang, [](int selected) {}, [&cfg](int selected) {
        std::string newLang = "auto";
        if (selected == 1) newLang = "ru";
        else if (selected == 2) newLang = "en-US";
        else if (selected == 3) newLang = "es";
        else if (selected == 4) newLang = "fr";
        else if (selected == 5) newLang = "de";
        else if (selected == 6) newLang = "it";
        else if (selected == 7) newLang = "pt-BR";
        else if (selected == 8) newLang = "zh-Hans";
        else if (selected == 9) newLang = "ja";

        if (newLang != cfg.getLanguage()) {
            cfg.setLanguage(newLang);
            cfg.setLastCatalogUpdateDate(""); // Force catalog refresh for new language
            cfg.save();

            // Switch locale in Borealis engine immediately
            brls::Application::setLocale(newLang);

            // Instantly recreate SettingsTab in the new locale
            brls::Application::popActivity(brls::TransitionAnimation::NONE, []() {
                brls::Application::pushActivity(new ui::SettingsTab(), brls::TransitionAnimation::NONE);
            });
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

    // Принудительная переустановка приложения
    auto* forceAppUpdateCell = new brls::DetailCell();
    forceAppUpdateCell->setText("app/settings/force_app_update"_i18n);
    forceAppUpdateCell->setDetailText("app/settings/force_app_update_desc"_i18n);
    forceAppUpdateCell->registerClickAction([&cfg](brls::View* view) {
        brls::Application::notify("app/settings/force_app_update_fetch"_i18n);
        std::string updateUrl = cfg.getEffectiveAppUpdateUrl();
        brls::async([updateUrl]() {
            net::HttpClient http;
            auto res = http.httpGet(updateUrl);
            if (res.status_code == 200 && !res.body.empty()) {
                try {
                    auto j = nlohmann::json::parse(res.body);
                    std::string version;
                    std::string url;

                    if (j.contains("tag_name")) {
                        version = j.value("tag_name", "");
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
                        version = j.value("version", "");
                        url = j.value("url", "");
                    }

                    if (!url.empty()) {
                        brls::sync([url, version]() {
                            std::string promptVer = !version.empty() ? version : "latest";
                            std::string msg = brls::getStr("app/settings/force_app_update_confirm", promptVer);
                            brls::Dialog* dialog = new brls::Dialog(msg);
                            dialog->addButton("app/common/yes"_i18n, [url, version]() {
                                downloadAndInstallAppUpdate(url, version);
                            });
                            dialog->addButton("app/common/no"_i18n, []() {});
                            dialog->open();
                        });
                        return;
                    }
                } catch (const std::exception& e) {
                    util::logLine("forceAppUpdate: parse error: " + std::string(e.what()));
                }
            } else {
                util::logLine("forceAppUpdate: fetch failed, status=" + std::to_string(res.status_code));
            }

            brls::sync([]() {
                brls::Dialog* errDialog = new brls::Dialog("app/settings/update_check_failed"_i18n);
                errDialog->addButton("app/common/ok"_i18n, []() {});
                errDialog->open();
            });
        });
        return true;
    });
    box->addView(forceAppUpdateCell);

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
                    if (text == "default" || text == "reset") {
                        cfg.setCatalogSourceUrl("");
                    } else {
                        cfg.setCatalogSourceUrl(text);
                    }
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

    // Обновление каталога игр (принудительное обновление)
    auto* catalogUpdateCell = new brls::DetailCell();
    catalogUpdateCell->setText("app/settings/update_catalog_btn"_i18n);
    auto updateCatalogStatusDisplay = [catalogUpdateCell]() {
        auto snap = getCatalogSnapshot();
        size_t gameCount = snap ? snap->size() : 0;
        catalogUpdateCell->setDetailText(brls::getStr("app/catalog/games_count", std::to_string(gameCount)));
    };
    updateCatalogStatusDisplay();
    catalogUpdateCell->registerClickAction([updateCatalogStatusDisplay](brls::View* view) {
        if (catalog::CatalogUpdater::instance().isUpdateRunning()) {
            brls::Application::notify("app/catalog/updating_db"_i18n);
            return true;
        }

        auto* dialog = new brls::Dialog("app/settings/update_catalog_dialog_msg"_i18n);
        dialog->addButton("app/settings/update_catalog_fast_diff"_i18n, [updateCatalogStatusDisplay]() {
            catalog::CatalogUpdater::instance().startUpdate(
                catalog::CatalogUpdateMode::ForceDiff,
                [updateCatalogStatusDisplay](const catalog::CatalogUpdateResult& res) {
                    updateCatalogStatusDisplay();
                },
                true
            );
        });
        dialog->addButton("app/settings/update_catalog_full"_i18n, [updateCatalogStatusDisplay]() {
            catalog::CatalogUpdater::instance().startUpdate(
                catalog::CatalogUpdateMode::ForceFull,
                [updateCatalogStatusDisplay](const catalog::CatalogUpdateResult& res) {
                    updateCatalogStatusDisplay();
                },
                true
            );
        });
        dialog->addButton("app/common/cancel"_i18n, []() {});
        dialog->open();
        return true;
    });
    box->addView(catalogUpdateCell);

    // Telegram и GitHub (QR-коды)
    auto* communityCell = new brls::DetailCell();
    communityCell->setText("app/settings/links_cell_title"_i18n);
    communityCell->setDetailText("app/settings/links_cell_desc"_i18n);
    communityCell->registerClickAction([this](brls::View* view) {
        showCommunityDialog();
        return true;
    });
    box->addView(communityCell);

    return scroll;
}

brls::View* SettingsTab::buildAppearanceTab() {
    auto& tm = ThemeManager::instance();
    brls::Box* box = nullptr;
    brls::ScrollingFrame* scroll = makeTabBox(&box);

    // 1. Цветовая тема (Theme Preset)
    const auto& allThemes = tm.getAllThemes();
    std::vector<std::string> themeNames;
    int initialThemeIdx = 0;
    std::string curTheme = tm.getCurrentThemeId();
    for (size_t i = 0; i < allThemes.size(); ++i) {
        themeNames.push_back(brls::getStr(allThemes[i].nameKey));
        if (allThemes[i].id == curTheme) {
            initialThemeIdx = static_cast<int>(i);
        }
    }

    auto* themeCell = new brls::SelectorCell();
    themeCell->init("app/appearance/theme_title"_i18n, themeNames, initialThemeIdx, [](int selected) {}, [&tm, allThemes, this](int selected) {
        if (selected >= 0 && selected < static_cast<int>(allThemes.size())) {
            std::string newId = allThemes[selected].id;
            if (newId != tm.getCurrentThemeId()) {
                tm.setTheme(newId);
                brls::Application::notify("app/appearance/theme_applied"_i18n);
                // Instantly recreate SettingsTab to reflect updated theme colors in sidebar and controls
                brls::Application::popActivity(brls::TransitionAnimation::NONE, []() {
                    auto* newSettings = new ui::SettingsTab();
                    brls::Application::pushActivity(newSettings, brls::TransitionAnimation::NONE);
                });
            }
        }
    });
    box->addView(themeCell);

    // 2. Отображать нижнюю информационную панель в главном меню
    auto& cfg = config::ConfigManager::instance();
    auto* showDashboardCell = new brls::BooleanCell();
    showDashboardCell->init("app/appearance/show_bottom_dashboard"_i18n, cfg.getShowBottomDashboard(), [&cfg](bool value) {
        cfg.setShowBottomDashboard(value);
    });
    box->addView(showDashboardCell);

    // Размер шрифта (Font Scale)
    std::vector<std::string> fontScaleOptions = {
        "app/appearance/font_scale_0"_i18n,
        "app/appearance/font_scale_1"_i18n,
        "app/appearance/font_scale_2"_i18n
    };
    int initialFontScaleIdx = cfg.getFontScale();
    if (initialFontScaleIdx < 0 || initialFontScaleIdx >= static_cast<int>(fontScaleOptions.size())) {
        initialFontScaleIdx = 0;
    }

    auto* fontScaleCell = new brls::SelectorCell();
    fontScaleCell->init("app/appearance/font_scale_title"_i18n, fontScaleOptions, initialFontScaleIdx, [](int selected) {}, [&cfg](int selected) {
        if (selected != cfg.getFontScale()) {
            cfg.setFontScale(selected);
            brls::Application::notify("app/appearance/font_scale_applied"_i18n);
        }
    });
    box->addView(fontScaleCell);

    // 3. Фоновое изображение (Wallpaper Preset)
    std::vector<std::string> bgOptions;
    std::vector<std::string> bgModes;
    bgOptions.push_back("app/appearance/bg_auto"_i18n);
    bgModes.push_back("auto");
    for (const auto& th : allThemes) {
        bgOptions.push_back(brls::getStr(th.nameKey));
        bgModes.push_back(th.id);
    }
    bgOptions.push_back("app/appearance/bg_custom"_i18n);
    bgModes.push_back("custom");

    std::string curBgMode = tm.getBackgroundMode();
    int initialBgIdx = 0;
    for (size_t i = 0; i < bgModes.size(); ++i) {
        if (bgModes[i] == curBgMode) {
            initialBgIdx = static_cast<int>(i);
            break;
        }
    }

    auto* customFileCell = new brls::DetailCell();
    customFileCell->setText("app/appearance/choose_file"_i18n);

    auto updateCustomFileDisplay = [customFileCell, &tm]() {
        std::string p = tm.getCustomBackgroundPath();
        if (p.empty()) {
            p = "app/appearance/no_file_selected"_i18n;
        } else if (p.length() > 38) {
            p = "..." + p.substr(p.length() - 35);
        }
        customFileCell->setDetailText(p);
    };
    updateCustomFileDisplay();

    customFileCell->registerClickAction([updateCustomFileDisplay](brls::View* view) {
        brls::Application::pushActivity(new ui::BackgroundFilePicker());
        return true;
    });

    auto* bgCell = new brls::SelectorCell();
    bgCell->init("app/appearance/bg_title"_i18n, bgOptions, initialBgIdx, [](int selected) {}, [&tm, customFileCell, bgModes](int selected) {
        if (selected >= 0 && selected < static_cast<int>(bgModes.size())) {
            std::string mode = bgModes[selected];
            tm.setBackgroundMode(mode);
            customFileCell->setVisibility((mode == "custom") ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
            brls::Application::notify("app/appearance/bg_applied"_i18n);
        }
    });
    box->addView(bgCell);

    customFileCell->setVisibility((bgModes[initialBgIdx] == "custom") ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    box->addView(customFileCell);

    // Размытие фона (Background Blur)
    std::vector<std::string> blurOptions = {
        "app/appearance/bg_blur_0"_i18n,
        "app/appearance/bg_blur_1"_i18n,
        "app/appearance/bg_blur_2"_i18n,
        "app/appearance/bg_blur_3"_i18n
    };
    int initialBlurIdx = tm.getBackgroundBlurLevel();
    if (initialBlurIdx < 0 || initialBlurIdx >= static_cast<int>(blurOptions.size())) initialBlurIdx = 0;

    auto* bgBlurCell = new brls::SelectorCell();
    bgBlurCell->init("app/appearance/bg_blur_title"_i18n, blurOptions, initialBlurIdx, [](int selected) {}, [&tm](int selected) {
        tm.setBackgroundBlurLevel(selected);
        brls::Application::notify("app/appearance/bg_blur_applied"_i18n);
    });
    box->addView(bgBlurCell);

    // Затемнение фона в главном меню (Background Dimming)
    std::vector<std::string> dimOptions = {
        "app/appearance/bg_dim_0"_i18n,
        "app/appearance/bg_dim_1"_i18n,
        "app/appearance/bg_dim_2"_i18n,
        "app/appearance/bg_dim_3"_i18n,
        "app/appearance/bg_dim_4"_i18n
    };
    int initialDimIdx = tm.getBackgroundDimLevel();
    if (initialDimIdx < 0 || initialDimIdx >= static_cast<int>(dimOptions.size())) initialDimIdx = 2;

    auto* bgDimCell = new brls::SelectorCell();
    bgDimCell->init("app/appearance/bg_dim_title"_i18n, dimOptions, initialDimIdx, [](int selected) {}, [&tm](int selected) {
        tm.setBackgroundDimLevel(selected);
        brls::Application::notify("app/appearance/bg_dim_applied"_i18n);
    });
    box->addView(bgDimCell);

    // 3. Сброс фона на стандартный
    auto* resetBgCell = new brls::DetailCell();
    resetBgCell->setText("app/appearance/reset_bg"_i18n);
    resetBgCell->registerClickAction([this, &tm](brls::View* view) {
        tm.setBackgroundMode("auto");
        tm.setCustomBackgroundPath("");
        tm.setBackgroundBlurLevel(0);
        tm.setBackgroundDimLevel(2);
        brls::Application::notify("app/appearance/bg_reset_done"_i18n);
        brls::Application::popActivity(brls::TransitionAnimation::NONE, []() {
            brls::Application::pushActivity(new ui::SettingsTab(), brls::TransitionAnimation::NONE);
        });
        return true;
    });
    box->addView(resetBgCell);

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

brls::View* SettingsTab::buildRetroTab() {
    auto& cfg = config::ConfigManager::instance();
    brls::Box* box = nullptr;
    brls::ScrollingFrame* scroll = makeTabBox(&box);

    // 1. Base ROMs folder selection
    std::vector<std::string> folderOptions = {
        "app/settings/retro_folder_retroarch"_i18n,
        "app/settings/retro_folder_downloads"_i18n,
        "app/settings/retro_folder_custom"_i18n
    };
    int initialFolderIdx = 0;
    std::string currentMode = cfg.getRetroRomsMode();
    if (currentMode == "downloads") initialFolderIdx = 1;
    else if (currentMode == "custom") initialFolderIdx = 2;
    else initialFolderIdx = 0;

    auto* customFolderCell = new brls::DetailCell();
    customFolderCell->setText("app/settings/retro_custom_folder"_i18n);

    auto updateCustomFolderDisplay = [customFolderCell, &cfg]() {
        std::string p = cfg.getRetroCustomPath();
        if (p.empty()) {
            p = "sdmc:/roms/";
        }
        customFolderCell->setDetailText(p);
    };
    updateCustomFolderDisplay();

    customFolderCell->registerClickAction([customFolderCell, updateCustomFolderDisplay, &cfg](brls::View* view) {
        brls::Application::getImeManager()->openForText(
            [updateCustomFolderDisplay, &cfg](std::string text) {
                if (!text.empty()) {
                    cfg.setRetroCustomPath(text);
                    cfg.save();
                    updateCustomFolderDisplay();
                }
            },
            "app/settings/retro_custom_folder_dialog"_i18n,
            "app/settings/retro_custom_folder_hint"_i18n,
            255,
            cfg.getRetroCustomPath(),
            0
        );
        return true;
    });

    auto* folderCell = new brls::SelectorCell();
    folderCell->init("app/settings/retro_roms_folder"_i18n, folderOptions, initialFolderIdx, [](int selected) {}, [&cfg, customFolderCell](int selected) {
        if (selected == 1) {
            cfg.setRetroRomsMode("downloads");
            customFolderCell->setVisibility(brls::Visibility::GONE);
        } else if (selected == 2) {
            cfg.setRetroRomsMode("custom");
            customFolderCell->setVisibility(brls::Visibility::VISIBLE);
        } else {
            cfg.setRetroRomsMode("retroarch");
            customFolderCell->setVisibility(brls::Visibility::GONE);
        }
        cfg.save();
    });
    box->addView(folderCell);

    customFolderCell->setVisibility((initialFolderIdx == 2) ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    box->addView(customFolderCell);

    // 2. Auto-extract toggle
    auto* autoExtractCell = new brls::BooleanCell();
    autoExtractCell->init("app/settings/retro_auto_extract"_i18n, cfg.getRetroAutoExtract(), [&cfg](bool value) {
        cfg.setRetroAutoExtract(value);
        cfg.save();
    });
    box->addView(autoExtractCell);

    // 3. Romset download mode
    std::vector<std::string> romsetOptions = {
        "app/settings/retro_romset_full"_i18n,
        "app/settings/retro_romset_select"_i18n
    };
    int initialRomsetIdx = (cfg.getRetroRomsetMode() == "select") ? 1 : 0;

    auto* romsetModeCell = new brls::SelectorCell();
    romsetModeCell->init("app/settings/retro_romset_mode"_i18n, romsetOptions, initialRomsetIdx, [&cfg](int selected) {
        if (selected == 1) {
            cfg.setRetroRomsetMode("select");
        } else {
            cfg.setRetroRomsetMode("full");
        }
        cfg.save();
    });
    box->addView(romsetModeCell);

    // 4. Update retro game databases
    auto* updateCatalogsCell = new brls::DetailCell();
    updateCatalogsCell->setText("app/settings/retro_update_catalogs"_i18n);

    auto updateDetailText = [updateCatalogsCell]() {
        int count = catalog::RetroCatalogManager::instance().getTotalCachedGamesCount();
        if (count > 0) {
            updateCatalogsCell->setDetailText(brls::getStr("app/settings/retro_update_catalogs_cached", std::to_string(count)));
        } else {
            updateCatalogsCell->setDetailText("app/settings/retro_update_catalogs_empty"_i18n);
        }
    };
    updateDetailText();

    updateCatalogsCell->registerClickAction([updateDetailText](brls::View* view) {
        showRetroCatalogUpdateDialog([updateDetailText](int updatedCount) {
            updateDetailText();
        });
        return true;
    });
    box->addView(updateCatalogsCell);

    return scroll;
}

brls::View* SettingsTab::buildStorageTab() {
    auto* scroll = new brls::ScrollingFrame();
    scroll->setWidth(brls::View::AUTO);
    scroll->setHeight(brls::View::AUTO);
    scroll->setContentView(new StorageTabView());
    return scroll;
}

brls::View* SettingsTab::buildAboutTab() {
    brls::Box* box = nullptr;
    brls::ScrollingFrame* scroll = makeTabBox(&box);

    // Header Box
    brls::Box* headerBox = new brls::Box(brls::Axis::COLUMN);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setMarginBottom(16.0f);

    brls::Label* titleLabel = new brls::Label();
    titleLabel->setText(std::string("TorrentShopNX v") + config::ConfigManager::APP_VERSION);
    titleLabel->setFontSize(26.0f);
    titleLabel->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    titleLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    titleLabel->setMarginBottom(4.0f);
    headerBox->addView(titleLabel);

    brls::Label* descLabel = new brls::Label();
    descLabel->setText("app/settings/about_desc"_i18n);
    descLabel->setFontSize(14.5f);
    descLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    descLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    descLabel->setMarginBottom(4.0f);
    headerBox->addView(descLabel);

    brls::Label* authorLabel = new brls::Label();
    authorLabel->setText("app/settings/about_author"_i18n);
    authorLabel->setFontSize(13.0f);
    authorLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    authorLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    authorLabel->setMarginBottom(10.0f);
    headerBox->addView(authorLabel);

    brls::Label* hintLabel = new brls::Label();
    hintLabel->setText("app/settings/community_hint"_i18n);
    hintLabel->setFontSize(13.5f);
    hintLabel->setTextColor(ThemeManager::instance().getTextAccentColor());
    hintLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    headerBox->addView(hintLabel);

    box->addView(headerBox);

    // Row of 2 QR cards
    brls::Box* cardsRow = new brls::Box(brls::Axis::ROW);
    cardsRow->setJustifyContent(brls::JustifyContent::CENTER);
    cardsRow->setAlignItems(brls::AlignItems::CENTER);
    cardsRow->setWidth(brls::View::AUTO);
    cardsRow->setHeight(brls::View::AUTO);

    auto createQrCard = [](const std::string& title, const std::string& desc,
                           const std::string& url, NVGcolor accentColor, bool isLeft) -> brls::Box* {
        brls::Box* card = new brls::Box(brls::Axis::COLUMN);
        card->setWidth(400.0f);
        card->setAlignItems(brls::AlignItems::CENTER);
        card->setBackgroundColor(ThemeManager::instance().isCurrentThemeLight() ? nvgRGBA(255, 255, 255, 245) : nvgRGBA(34, 38, 48, 220));
        card->setBorderThickness(1.0f);
        card->setBorderColor(ThemeManager::instance().getCardBorderColor());
        card->setCornerRadius(16.0f);
        card->setPadding(18.0f, 18.0f, 16.0f, 18.0f);
        if (isLeft) {
            card->setMarginRight(14.0f);
        } else {
            card->setMarginLeft(14.0f);
        }
        card->setFocusable(true);

        brls::Label* lblTitle = new brls::Label();
        lblTitle->setText(title);
        lblTitle->setFontSize(20.0f);
        lblTitle->setTextColor(accentColor);
        lblTitle->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        lblTitle->setMarginBottom(3.0f);
        card->addView(lblTitle);

        brls::Label* lblDesc = new brls::Label();
        lblDesc->setText(desc);
        lblDesc->setFontSize(12.5f);
        lblDesc->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        lblDesc->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        lblDesc->setMarginBottom(12.0f);
        card->addView(lblDesc);

        brls::Box* qrContainer = new brls::Box();
        qrContainer->setWidth(200.0f);
        qrContainer->setHeight(200.0f);
        qrContainer->setBackgroundColor(nvgRGB(255, 255, 255));
        qrContainer->setCornerRadius(12.0f);
        qrContainer->setJustifyContent(brls::JustifyContent::CENTER);
        qrContainer->setAlignItems(brls::AlignItems::CENTER);
        qrContainer->setPadding(10.0f, 10.0f, 10.0f, 10.0f);
        qrContainer->setMarginBottom(12.0f);

        QrCodeView* qrView = new QrCodeView();
        qrView->setWidth(180.0f);
        qrView->setHeight(180.0f);
        qrView->setContent(url);
        qrContainer->addView(qrView);
        card->addView(qrContainer);

        brls::Box* urlBox = new brls::Box();
        urlBox->setWidth(350.0f);
        urlBox->setBackgroundColor(ThemeManager::instance().isCurrentThemeLight() ? nvgRGBA(242, 246, 252, 240) : nvgRGBA(18, 20, 26, 220));
        urlBox->setCornerRadius(8.0f);
        urlBox->setPadding(6.0f, 10.0f, 6.0f, 10.0f);
        urlBox->setAlignItems(brls::AlignItems::CENTER);
        urlBox->setMarginBottom(8.0f);

        brls::Label* lblUrl = new brls::Label();
        lblUrl->setText(url);
        lblUrl->setFontSize(12.0f);
        lblUrl->setTextColor(accentColor);
        lblUrl->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        urlBox->addView(lblUrl);
        card->addView(urlBox);

        brls::Label* hintAction = new brls::Label();
        hintAction->setText("app/settings/qr_press_hint"_i18n);
        hintAction->setFontSize(11.5f);
        hintAction->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        hintAction->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        card->addView(hintAction);

        card->registerClickAction([title, url, desc](brls::View* view) {
            QrDialog::open(title, url, desc);
            return true;
        });

        return card;
    };

    cardsRow->addView(createQrCard("app/settings/telegram_title"_i18n,
                                   "app/settings/telegram_desc"_i18n,
                                   "https://t.me/TorrentShopNX",
                                   nvgRGB(56, 170, 245),
                                   true));
    cardsRow->addView(createQrCard("app/settings/github_title"_i18n,
                                   "app/settings/github_desc"_i18n,
                                   "https://github.com/Langegen/TorrentShopNX",
                                   ThemeManager::instance().isCurrentThemeLight() ? nvgRGB(36, 42, 54) : nvgRGB(235, 240, 245),
                                   false));

    box->addView(cardsRow);

    return scroll;
}

void SettingsTab::showCommunityDialog() {
    brls::Box* content = new brls::Box(brls::Axis::COLUMN);
    content->setAlignItems(brls::AlignItems::CENTER);
    content->setWidth(680.0f);
    content->setPadding(10.0f, 10.0f, 10.0f, 10.0f);

    brls::Label* titleLabel = new brls::Label();
    titleLabel->setText("app/settings/links_dialog_title"_i18n);
    titleLabel->setFontSize(20.0f);
    titleLabel->setTextColor(ThemeManager::instance().getTextPrimaryColor());
    titleLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    titleLabel->setMarginBottom(4.0f);
    content->addView(titleLabel);

    brls::Label* hintLabel = new brls::Label();
    hintLabel->setText("app/settings/community_hint"_i18n);
    hintLabel->setFontSize(13.5f);
    hintLabel->setTextColor(ThemeManager::instance().getTextSecondaryColor());
    hintLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    hintLabel->setMarginBottom(16.0f);
    content->addView(hintLabel);

    brls::Box* row = new brls::Box(brls::Axis::ROW);
    row->setJustifyContent(brls::JustifyContent::CENTER);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setWidth(brls::View::AUTO);
    row->setHeight(brls::View::AUTO);

    auto addQrCol = [row](const std::string& name, const std::string& url,
                          const std::string& desc, NVGcolor titleColor) {
        brls::Box* col = new brls::Box(brls::Axis::COLUMN);
        col->setAlignItems(brls::AlignItems::CENTER);
        col->setWidth(300.0f);
        col->setMarginLeft(10.0f);
        col->setMarginRight(10.0f);

        brls::Label* lblName = new brls::Label();
        lblName->setText(name);
        lblName->setFontSize(17.0f);
        lblName->setTextColor(titleColor);
        lblName->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        lblName->setMarginBottom(2.0f);
        col->addView(lblName);

        brls::Label* lblDesc = new brls::Label();
        lblDesc->setText(desc);
        lblDesc->setFontSize(11.5f);
        lblDesc->setTextColor(ThemeManager::instance().getTextSecondaryColor());
        lblDesc->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        lblDesc->setMarginBottom(8.0f);
        col->addView(lblDesc);

        brls::Box* qrBox = new brls::Box();
        qrBox->setWidth(170.0f);
        qrBox->setHeight(170.0f);
        qrBox->setBackgroundColor(nvgRGB(255, 255, 255));
        qrBox->setCornerRadius(10.0f);
        qrBox->setJustifyContent(brls::JustifyContent::CENTER);
        qrBox->setAlignItems(brls::AlignItems::CENTER);
        qrBox->setPadding(8.0f, 8.0f, 8.0f, 8.0f);
        qrBox->setMarginBottom(8.0f);

        QrCodeView* qr = new QrCodeView();
        qr->setWidth(154.0f);
        qr->setHeight(154.0f);
        qr->setContent(url);
        qrBox->addView(qr);
        col->addView(qrBox);

        brls::Label* lblUrl = new brls::Label();
        lblUrl->setText(url);
        lblUrl->setFontSize(11.0f);
        lblUrl->setTextColor(titleColor);
        lblUrl->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        col->addView(lblUrl);

        row->addView(col);
    };

    addQrCol("app/settings/telegram_title"_i18n,
             "https://t.me/TorrentShopNX",
             "app/settings/telegram_desc"_i18n,
             nvgRGB(56, 170, 245));

    addQrCol("app/settings/github_title"_i18n,
             "https://github.com/Langegen/TorrentShopNX",
             "app/settings/github_desc"_i18n,
             ThemeManager::instance().isCurrentThemeLight() ? nvgRGB(36, 42, 54) : nvgRGB(230, 235, 245));

    content->addView(row);

    brls::Dialog* dialog = new brls::Dialog(content);
    dialog->setCancelable(true);
    dialog->addButton("app/common/ok"_i18n, []() {});
    dialog->open();
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