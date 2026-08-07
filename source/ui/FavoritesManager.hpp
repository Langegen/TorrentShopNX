#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include "../GameData.hpp"

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

    bool isFavorite(const std::string& key_or_topic_id) const;
    bool isFavorite(const Game& game) const;

    void addFavorite(const Game& game);
    void addFavorite(const std::string& topic_id);

    void removeFavorite(const std::string& key_or_topic_id);
    void removeFavorite(const Game& game);

    bool toggleFavorite(const Game& game);
    bool toggleFavorite(const std::string& topic_id);

    void syncLegacyFavorites(const std::vector<Game>& catalog_games);

    const std::vector<Game>& getFavorites() const { return favorite_games_; }

private:
    FavoritesManager() = default;

    std::string getGameKey(const Game& g) const;

    std::string filepath_;
    std::vector<Game> favorite_games_;
    std::unordered_set<std::string> legacy_topic_ids_;
};

} // namespace catalog
