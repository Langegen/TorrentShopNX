#pragma once

#include <string>
#include <unordered_set>

namespace catalog {

class FavoritesManager {
public:
    static FavoritesManager& instance() {
        static FavoritesManager* inst = new FavoritesManager();
        return *inst;
    }

    void init(const std::string& path);
    bool load();
    bool save();

    bool isFavorite(const std::string& topic_id) const;
    void addFavorite(const std::string& topic_id);
    void removeFavorite(const std::string& topic_id);
    bool toggleFavorite(const std::string& topic_id);

    const std::unordered_set<std::string>& getFavorites() const { return favorites_; }

private:
    FavoritesManager() = default;
    
    std::string filepath_;
    std::unordered_set<std::string> favorites_;
};

} // namespace catalog
