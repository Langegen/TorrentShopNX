#include "config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <ctime>
#include "../utils/log.h"

namespace config {

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
                     std::string& last_catalog_update_date,
                     std::string& install_location,
                     std::string& app_update_url,
                     bool& auto_app_update,
                     std::string& last_app_update_check_date) {
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
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    out.assign((std::istreambuf_iterator<char>(file)),
               std::istreambuf_iterator<char>());
    return true;
}

} // namespace

ConfigManager& ConfigManager::instance() {
    static ConfigManager* inst = new ConfigManager();
    return *inst;
}

ConfigManager::ConfigManager() {
    config_path_ = "sdmc:/switch/TorrentShopNX/config.ini";
    legacy_config_path_ = "sdmc:/switch/TorrentShopNX/config.txt";
    torrserver_url_ = "http://192.168.1.100:8090";
    catalog_source_url_.clear();
    data_mode_ = "local_client";
    keep_awake_during_downloads_ = true;
    last_catalog_update_date_.clear();
    install_location_ = "auto";
    app_update_url_ = "https://api.github.com/repos/Langegen/TorrentShopNX/releases/latest";
    auto_app_update_ = true;
    last_app_update_check_date_.clear();
    load();
}

void ConfigManager::load() {
    std::string body;
    if (readWholeFile(config_path_, body)) {
        parseConfigBody(body, torrserver_url_, catalog_source_url_, data_mode_,
                        keep_awake_during_downloads_, last_catalog_update_date_, install_location_, app_update_url_,
                        auto_app_update_, last_app_update_check_date_);
        if (data_mode_ != "torrserver" && data_mode_ != "local_client") data_mode_ = "local_client";
        if (install_location_ != "sd" && install_location_ != "nand") install_location_ = "auto";
        util::logLine("config: loaded config.ini, TorrServer URL: " + torrserver_url_ + ", install_location: " + install_location_);
        return;
    }

    // Backward compatibility: migrate old config.txt on first run.
    if (readWholeFile(legacy_config_path_, body)) {
        parseConfigBody(body, torrserver_url_, catalog_source_url_, data_mode_,
                        keep_awake_during_downloads_, last_catalog_update_date_, install_location_, app_update_url_,
                        auto_app_update_, last_app_update_check_date_);
        if (data_mode_ != "torrserver" && data_mode_ != "local_client") data_mode_ = "local_client";
        if (install_location_ != "sd" && install_location_ != "nand") install_location_ = "auto";
        util::logLine("config: loaded legacy config.txt, migrating to config.ini");
        save();
        return;
    }

    util::logLine("config: config files not found, using defaults");
    save();
}

void ConfigManager::save() {
    std::filesystem::path dir = "sdmc:/switch/TorrentShopNX";
    if (!std::filesystem::exists(dir)) {
        std::filesystem::create_directories(dir);
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
    file << "last_catalog_update_date=" << last_catalog_update_date_ << "\n";
    file << "install_location=" << install_location_ << "\n";
    file << "app_update_url=" << app_update_url_ << "\n";
    file << "auto_app_update=" << (auto_app_update_ ? "true" : "false") << "\n";
    file << "last_app_update_check_date=" << last_app_update_check_date_ << "\n";
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
    if (catalog_source_url_.empty()) {
        return DEFAULT_CATALOG_URL;
    }
    return catalog_source_url_;
}

void ConfigManager::setCatalogSourceUrl(const std::string& url) {
    catalog_source_url_ = url;
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
#if defined(_MSC_VER)
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

} // namespace config
