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

    const std::string& getCatalogSourceUrl() const;
    void setCatalogSourceUrl(const std::string& url);

    const std::string& getDataMode() const;
    void setDataMode(const std::string& mode);

    bool getKeepAwakeDuringDownloads() const;
    void setKeepAwakeDuringDownloads(bool enabled);

    const std::string& getLastCatalogUpdateDate() const;
    void setLastCatalogUpdateDate(const std::string& date_yyyy_mm_dd);
    bool shouldUpdateCatalogToday() const;
    static std::string currentDateString();

private:
    ConfigManager();
    ~ConfigManager() = default;

    std::string torrserver_url_;
    std::string catalog_source_url_;
    std::string data_mode_;
    bool keep_awake_during_downloads_;
    std::string last_catalog_update_date_;
    std::string config_path_;
    std::string legacy_config_path_;
};

} // namespace config
