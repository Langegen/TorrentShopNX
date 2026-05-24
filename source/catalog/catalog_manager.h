#pragma once

#include <string>
#include <vector>

namespace catalog {

struct CatalogEntry {
    std::string title;
    std::string size;
    std::string magnet;
    std::string category;
    std::string description;
    std::string icon;
};

struct CatalogSource {
    std::string name;
    std::string type; // json, rss, magnet, torrent
    std::string url;
};

class CatalogManager {
public:
    bool loadCatalogFromFile(const std::string& catalog_path);
    bool loadSources(const std::string& sources_path);
    bool loadSourcesWithFallback(const std::string& primary_path, const std::string& fallback_path);
    void updateCatalogs();
    void mergeCatalogEntries();
    std::vector<CatalogEntry> searchCatalog(const std::string& query) const;

    const std::vector<CatalogEntry>& entries() const { return merged_entries_; }
    const std::vector<CatalogSource>& sources() const { return sources_; }

private:
    std::vector<CatalogSource> sources_;
    std::vector<CatalogEntry> merged_entries_;
    int cache_ttl_seconds_ = 1800;

    std::vector<CatalogEntry> parseJsonCatalog(const std::string& body);
    CatalogEntry parseEntryObject(const std::string& obj);
    std::string extractJsonValue(const std::string& obj, const std::string& key);
    std::vector<CatalogSource> parseSourcesJson(const std::string& body);
    std::string toLower(const std::string& s) const;
    std::string cachePathFor(const CatalogSource& s) const;
    bool loadCachedBody(const CatalogSource& s, std::string& out_body) const;
    void saveCache(const CatalogSource& s, const std::string& body) const;
    bool ensureCacheDir() const;
};

} // namespace catalog
