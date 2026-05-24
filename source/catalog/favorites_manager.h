#pragma once

#include <string>
#include <vector>
#include "catalog_manager.h"

namespace catalog {

class FavoritesManager {
public:
    static FavoritesManager& instance() {
        static FavoritesManager inst;
        return inst;
    }

    void init(const std::string& path);
    bool load();
    bool save();

    bool isFavorite(const std::string& title) const;
    void addFavorite(const CatalogEntry& entry);
    void removeFavorite(const std::string& title);
    bool toggleFavorite(const CatalogEntry& entry);

    const std::vector<CatalogEntry>& getFavorites() const { return favorites_; }

private:
    FavoritesManager() = default;
    
    std::string filepath_;
    std::vector<CatalogEntry> favorites_;
};

} // namespace catalog
