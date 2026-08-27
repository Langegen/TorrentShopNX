#include "config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <ctime>
#include "../utils/log.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace config {

static std::string normalizeCatalogUrl(std::string url) {
    if (url.empty()) return url;
    // If github.com/.../blob/..., convert to raw.githubusercontent.com/.../
    size_t ghPos = url.find("github.com/");
    if (ghPos != std::string::npos) {
        size_t blobPos = url.find("/blob/", ghPos);
        if (blobPos != std::string::npos) {
            url.replace(blobPos, 6, "/");
            ghPos = url.find("github.com/");
            if (ghPos != std::string::npos) {
                url.replace(ghPos, 10, "raw.githubusercontent.com");
            }
        }
    }
    return url;
}

namespace {

std::string trim(const std::string& in) {
    size_t b = 0;
    while (b < in.size() && std::isspace(static_cast<unsigned char>(in[b]))) ++b;
    size_t e = in.size();
    while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
    return in.substr(b, e - b);
}

bool parseConfigBody(const std::string& body,
                     std::string& torrserver_url,
                     std::string& catalog_source_url,
                     std::string& data_mode,
                     bool& keep_awake_during_downloads,
                     int& backlight_timeout,
                     bool& cache_cover_thumbnails,
                     int& listen_port,
                     std::string& last_catalog_update_date,
                     std::string& install_location,
                     std::string& app_update_url,
                     bool& auto_app_update,
                     std::string& last_app_update_check_date,
                     std::string& language) {
    bool parsed_known_keys = false;
    std::string legacy_single_value;

    auto parseBool = [](std::string value, bool default_value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
        if (value == "0" || value == "false" || value == "no" || value == "off") return false;
        return default_value;
    };

    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        std::string line = (end == std::string::npos) ? body.substr(pos) : body.substr(pos, end - pos);
        line = trim(line);
        if (!line.empty() && line[0] != '#' && line[0] != ';' && line[0] != '[') {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = trim(line.substr(0, eq));
                std::string val = trim(line.substr(eq + 1));
                if (key == "torrserver_url") {
                    torrserver_url = val;
                    parsed_known_keys = true;
                } else if (key == "catalog_source_url") {
                    catalog_source_url = val;
                    parsed_known_keys = true;
                } else if (key == "data_mode") {
                    data_mode = val;
                    parsed_known_keys = true;
                } else if (key == "keep_awake_during_downloads") {
                    keep_awake_during_downloads = parseBool(val, keep_awake_during_downloads);
                    parsed_known_keys = true;
                } else if (key == "backlight_timeout") {
                    int t = std::atoi(val.c_str());
                    if (t == 15 || t == 30 || t == 60 || t == 120) backlight_timeout = t;
                    else backlight_timeout = 0;
                    parsed_known_keys = true;
                } else if (key == "cache_cover_thumbnails") {
                    cache_cover_thumbnails = parseBool(val, cache_cover_thumbnails);
                    parsed_known_keys = true;
                } else if (key == "listen_port") {
                    int p = std::atoi(val.c_str());
                    if (p > 0 && p < 65536) listen_port = p;
                    parsed_known_keys = true;
                } else if (key == "last_catalog_update_date") {
                    last_catalog_update_date = val;
                    parsed_known_keys = true;
                } else if (key == "install_location") {
                    install_location = val;
                    parsed_known_keys = true;
                } else if (key == "app_update_url") {
                    app_update_url = val;
                    parsed_known_keys = true;
                } else if (key == "auto_app_update") {
                    auto_app_update = parseBool(val, auto_app_update);
                    parsed_known_keys = true;
                } else if (key == "last_app_update_check_date") {
                    last_app_update_check_date = val;
                    parsed_known_keys = true;
                } else if (key == "language") {
                    language = val;
                    parsed_known_keys = true;
                }
            } else if (legacy_single_value.empty()) {
                // Legacy format: config.txt contained only TorrServer URL in one line.
                legacy_single_value = line;
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }

    if (!parsed_known_keys && !legacy_single_value.empty()) {
        torrserver_url = legacy_single_value;
        parsed_known_keys = true;
    }
    return parsed_known_keys;
}

bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        in.read(&out[0], size);
        if (!in && in.gcount() != size) {
            out.resize(static_cast<size_t>(in.gcount()));
        }
    }
    return true;
}

} // namespace

ConfigManager& ConfigManager::instance() {
    static ConfigManager* inst = new ConfigManager();
    return *inst;
}

ConfigManager::ConfigManager() {
#ifdef __SWITCH__
    config_path_ = "sdmc:/switch/TorrentShopNX/config.ini";
    legacy_config_path_ = "sdmc:/switch/TorrentShopNX/config.txt";
#else
    config_path_ = "config.ini";
    legacy_config_path_ = "config.txt";
#endif
    // PC tests override the path with an env var (the sdmc: path is unusable
    // outside the console).
    if (const char* p = std::getenv("TSNX_CONFIG_PATH")) {
        if (p[0]) config_path_ = p;
    }
    torrserver_url_ = "http://192.168.1.100:8090";
    catalog_source_url_.clear();
    data_mode_ = "local_client";
    keep_awake_during_downloads_ = true;
    backlight_timeout_ = 0;
    cache_cover_thumbnails_ = false;
    listen_port_ = 6882;
    last_catalog_update_date_.clear();
    install_location_ = "auto";
    app_update_url_ = "https://api.github.com/repos/Langegen/TorrentShopNX/releases/latest";
    auto_app_update_ = true;
    last_app_update_check_date_.clear();
    language_ = "auto";
    load();
}

void ConfigManager::load() {
    std::string body;
    if (readWholeFile(config_path_, body)) {
        parseConfigBody(body, torrserver_url_, catalog_source_url_, data_mode_,
                        keep_awake_during_downloads_, backlight_timeout_, cache_cover_thumbnails_, listen_port_,
                        last_catalog_update_date_, install_location_, app_update_url_,
                        auto_app_update_, last_app_update_check_date_, language_);
        if (data_mode_ != "torrserver" && data_mode_ != "local_client") data_mode_ = "local_client";
        if (install_location_ != "sd" && install_location_ != "nand") install_location_ = "auto";
        if (language_ != "ru" && language_ != "en-US" && language_ != "en") language_ = "auto";
        util::logLine("config: loaded config.ini, TorrServer URL: " + torrserver_url_ + ", install_location: " + install_location_ + ", language: " + language_);
        return;
    }

    // Backward compatibility: migrate old config.txt on first run.
    if (readWholeFile(legacy_config_path_, body)) {
        parseConfigBody(body, torrserver_url_, catalog_source_url_, data_mode_,
                        keep_awake_during_downloads_, backlight_timeout_, cache_cover_thumbnails_, listen_port_,
                        last_catalog_update_date_, install_location_, app_update_url_,
                        auto_app_update_, last_app_update_check_date_, language_);
        if (data_mode_ != "torrserver" && data_mode_ != "local_client") data_mode_ = "local_client";
        if (install_location_ != "sd" && install_location_ != "nand") install_location_ = "auto";
        if (language_ != "ru" && language_ != "en-US" && language_ != "en") language_ = "auto";
        util::logLine("config: loaded legacy config.txt, migrating to config.ini");
        save();
        return;
    }

    util::logLine("config: config files not found, using defaults");
    save();
}

void ConfigManager::save() {
    try {
        std::filesystem::path dir = "sdmc:/switch/TorrentShopNX";
        if (config_path_.rfind("sdmc:/", 0) == 0 && !std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }
    } catch (...) {
        // PC builds have no sdmc: drive; nothing to create, saving is best-effort.
    }

    std::ofstream file(config_path_);
    if (!file.is_open()) {
        util::logLine("config: failed to open config for writing");
        return;
    }

    file << "[general]\n";
    file << "torrserver_url=" << torrserver_url_ << "\n";
    file << "catalog_source_url=" << catalog_source_url_ << "\n";
    file << "data_mode=" << data_mode_ << "\n";
    file << "keep_awake_during_downloads=" << (keep_awake_during_downloads_ ? "true" : "false") << "\n";
    file << "backlight_timeout=" << backlight_timeout_ << "\n";
    file << "cache_cover_thumbnails=" << (cache_cover_thumbnails_ ? "true" : "false") << "\n";
    file << "listen_port=" << listen_port_ << "\n";
    file << "last_catalog_update_date=" << last_catalog_update_date_ << "\n";
    file << "install_location=" << install_location_ << "\n";
    file << "app_update_url=" << app_update_url_ << "\n";
    file << "auto_app_update=" << (auto_app_update_ ? "true" : "false") << "\n";
    file << "last_app_update_check_date=" << last_app_update_check_date_ << "\n";
    file << "language=" << language_ << "\n";
    util::logLine("config: saved config.ini");
}

const std::string& ConfigManager::getTorrServerUrl() const {
    return torrserver_url_;
}

void ConfigManager::setTorrServerUrl(const std::string& url) {
    torrserver_url_ = url;
    save();
}

const std::string& ConfigManager::getCatalogSourceUrl() const {
    return catalog_source_url_;
}

std::string ConfigManager::getEffectiveCatalogSourceUrl() const {
    std::string url = normalizeCatalogUrl(catalog_source_url_);

    bool isCustom = !url.empty() &&
                    url != DEFAULT_CATALOG_URL_RU &&
                    url != DEFAULT_CATALOG_URL_EN &&
                    url != LEGACY_CATALOG_URL &&
                    url != "https://raw.githubusercontent.com/Langegen/switch-game-collection/main/RU_catalog.json" &&
                    url != "https://raw.githubusercontent.com/Langegen/switch-game-collection/main/EN_catalog.json" &&
                    url != "https://raw.githubusercontent.com/Langegen/switch-game-collection/refs/heads/main/RU_catalog.json" &&
                    url != "https://raw.githubusercontent.com/Langegen/switch-game-collection/refs/heads/main/EN_catalog.json";

    if (isCustom) {
        return url;
    }

    // Determine default catalog based on active / configured language
    std::string lang = language_;
    if (lang.empty() || lang == "auto") {
#ifdef __SWITCH__
        uint64_t languageCode = 0;
        if (R_SUCCEEDED(setGetSystemLanguage(&languageCode))) {
            char* languageName = (char*)&languageCode;
            lang = std::string(languageName);
        } else {
            lang = "en-US";
        }
#else
        lang = "ru";
#endif
    }

    if (lang == "ru" || lang.rfind("ru", 0) == 0) {
        return DEFAULT_CATALOG_URL_RU;
    }
    return DEFAULT_CATALOG_URL_EN;
}

void ConfigManager::setCatalogSourceUrl(const std::string& url) {
    catalog_source_url_ = normalizeCatalogUrl(url);
    save();
}

const std::string& ConfigManager::getDataMode() const {
    return data_mode_;
}

void ConfigManager::setDataMode(const std::string& mode) {
    std::string normalized = mode;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (normalized != "torrserver" && normalized != "local_client") {
        normalized = "local_client";
    }
    data_mode_ = normalized;
    save();
}

bool ConfigManager::getKeepAwakeDuringDownloads() const {
    return keep_awake_during_downloads_;
}

void ConfigManager::setKeepAwakeDuringDownloads(bool enabled) {
    keep_awake_during_downloads_ = enabled;
    save();
}

int ConfigManager::getBacklightTimeout() const {
    return backlight_timeout_;
}

void ConfigManager::setBacklightTimeout(int seconds) {
    if (seconds != 15 && seconds != 30 && seconds != 60 && seconds != 120) {
        backlight_timeout_ = 0;
    } else {
        backlight_timeout_ = seconds;
    }
    save();
}

int ConfigManager::getListenPort() const {
    return listen_port_;
}

void ConfigManager::setListenPort(int port) {
    if (port <= 0 || port >= 65536) return;
    listen_port_ = port;
    save();
}

const std::string& ConfigManager::getLastCatalogUpdateDate() const {
    return last_catalog_update_date_;
}

void ConfigManager::setLastCatalogUpdateDate(const std::string& date_yyyy_mm_dd) {
    last_catalog_update_date_ = date_yyyy_mm_dd;
    save();
}

std::string ConfigManager::currentDateString() {
    std::time_t now = std::time(nullptr);
    if (now <= 0) return "";

    std::tm tm_now = {};
#if defined(_MSC_VER) || defined(__MINGW32__)
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    char buf[16] = {0};
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_now) == 0) {
        return "";
    }
    return std::string(buf);
}

bool ConfigManager::shouldUpdateCatalogToday() const {
    const std::string today = currentDateString();
    if (today.empty()) return false;
    return last_catalog_update_date_ != today;
}

const std::string& ConfigManager::getInstallLocation() const {
    return install_location_;
}

void ConfigManager::setInstallLocation(const std::string& location) {
    install_location_ = location;
    save();
}

const std::string& ConfigManager::getAppUpdateUrl() const {
    return app_update_url_;
}

std::string ConfigManager::getEffectiveAppUpdateUrl() const {
    if (app_update_url_.empty()) {
        return DEFAULT_APP_UPDATE_URL;
    }
    return app_update_url_;
}

void ConfigManager::setAppUpdateUrl(const std::string& url) {
    app_update_url_ = url;
    save();
}

bool ConfigManager::getAutoAppUpdate() const {
    return auto_app_update_;
}

void ConfigManager::setAutoAppUpdate(bool enabled) {
    auto_app_update_ = enabled;
    save();
}

bool ConfigManager::getCacheCoverThumbnails() const {
    return cache_cover_thumbnails_;
}

void ConfigManager::setCacheCoverThumbnails(bool enabled) {
    cache_cover_thumbnails_ = enabled;
    save();
}

const std::string& ConfigManager::getLastAppUpdateCheckDate() const {
    return last_app_update_check_date_;
}

void ConfigManager::setLastAppUpdateCheckDate(const std::string& date_yyyy_mm_dd) {
    last_app_update_check_date_ = date_yyyy_mm_dd;
    save();
}

bool ConfigManager::shouldCheckAppUpdateToday() const {
    const std::string today = currentDateString();
    if (today.empty()) return false;
    return last_app_update_check_date_ != today;
}

const std::string& ConfigManager::getLanguage() const {
    return language_;
}

void ConfigManager::setLanguage(const std::string& lang) {
    if (lang != "ru" && lang != "en-US" && lang != "en") {
        language_ = "auto";
    } else {
        language_ = lang;
    }
    save();
}

} // namespace config
