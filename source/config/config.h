#pragma once

#include <string>

namespace config {

class ConfigManager {
public:
    static ConfigManager& instance();

    void load();
    void save();

    const std::string& getTorrServerUrl() const;
    void setTorrServerUrl(const std::string& url);

    static constexpr const char* DEFAULT_CATALOG_URL_RU = "https://raw.githubusercontent.com/Langegen/switch-game-collection/refs/heads/main/RU_catalog.json";
    static constexpr const char* DEFAULT_CATALOG_URL_EN = "https://raw.githubusercontent.com/Langegen/switch-game-collection/refs/heads/main/EN_catalog.json";
    static constexpr const char* DEFAULT_CATALOG_URL = DEFAULT_CATALOG_URL_RU;
    static constexpr const char* LEGACY_CATALOG_URL = "https://raw.githubusercontent.com/Langegen/switch-games/refs/heads/main/switch_games.json";

    const std::string& getCatalogSourceUrl() const;
    std::string getEffectiveCatalogSourceUrl() const;
    void setCatalogSourceUrl(const std::string& url);

    const std::string& getDataMode() const;
    void setDataMode(const std::string& mode);

    bool getKeepAwakeDuringDownloads() const;
    void setKeepAwakeDuringDownloads(bool enabled);

    // Screen backlight timeout in seconds during active downloads.
    // 0 = Manual (never turn off automatically), 15 = 15s, 30 = 30s, 60 = 60s, 120 = 120s.
    int getBacklightTimeout() const;
    void setBacklightTimeout(int seconds);

    // Listen port for incoming BitTorrent connections. Forward this port on
    // the router (TCP) to the console so firewalled seeders can dial in --
    // without it, only outbound-reachable peers are usable. Default 6882.
    int getListenPort() const;
    void setListenPort(int port);

    const std::string& getLastCatalogUpdateDate() const;
    void setLastCatalogUpdateDate(const std::string& date_yyyy_mm_dd);
    bool shouldUpdateCatalogToday() const;
    static std::string currentDateString();

    const std::string& getInstallLocation() const;
    void setInstallLocation(const std::string& location);

    static constexpr const char* DEFAULT_APP_UPDATE_URL = "https://api.github.com/repos/Langegen/TorrentShopNX/releases/latest";
    static constexpr const char* APP_VERSION = "2.6"; // Keep in sync with Makefile APP_VERSION

    const std::string& getAppUpdateUrl() const;
    std::string getEffectiveAppUpdateUrl() const;
    void setAppUpdateUrl(const std::string& url);

    bool getAutoAppUpdate() const;
    void setAutoAppUpdate(bool enabled);

    bool getCacheCoverThumbnails() const;
    void setCacheCoverThumbnails(bool enabled);

    const std::string& getLastAppUpdateCheckDate() const;
    void setLastAppUpdateCheckDate(const std::string& date_yyyy_mm_dd);
    bool shouldCheckAppUpdateToday() const;

    const std::string& getLanguage() const;
    void setLanguage(const std::string& lang);

private:
    ConfigManager();
    ~ConfigManager() = default;

    std::string torrserver_url_;
    std::string catalog_source_url_;
    std::string data_mode_;
    bool keep_awake_during_downloads_;
    int backlight_timeout_;
    bool cache_cover_thumbnails_;
    int listen_port_;
    std::string last_catalog_update_date_;
    std::string install_location_;
    std::string app_update_url_;
    bool auto_app_update_;
    std::string last_app_update_check_date_;
    std::string language_;
    std::string config_path_;
    std::string legacy_config_path_;
};

} // namespace config
