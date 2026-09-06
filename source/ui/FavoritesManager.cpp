#include "FavoritesManager.hpp"
#include <fstream>
#include <borealis/extern/nlohmann/json.hpp>
#include "../utils/log.h"

extern std::vector<Game> g_games;

namespace catalog {

std::string FavoritesManager::getGameKey(const Game& g) const {
    if (!g.topic_id.empty()) return g.topic_id;
    if (!g.magnet.empty()) return g.magnet;
    return g.title;
}

void FavoritesManager::init(const std::string& path) {
    filepath_ = path;
    load();
}

bool FavoritesManager::load() {
    favorite_games_.clear();
    legacy_topic_ids_.clear();

    std::ifstream in(filepath_);
    if (!in.is_open()) {
        util::logLine("favorites: could not open file for reading: " + filepath_);
        return false;
    }
    try {
        nlohmann::json j;
        in >> j;
        if (j.is_array()) {
            for (const auto& item : j) {
                if (item.is_object()) {
                    Game g = item.get<Game>();
                    if (!getGameKey(g).empty()) {
                        favorite_games_.push_back(g);
                    }
                } else if (item.is_string()) {
                    std::string id = item.get<std::string>();
                    if (!id.empty()) {
                        legacy_topic_ids_.insert(id);
                    }
                }
            }
        }
        util::logLine("favorites: loaded games_count=" + std::to_string(favorite_games_.size()) +
                      " legacy_count=" + std::to_string(legacy_topic_ids_.size()));
    } catch (...) {
        util::logLine("favorites: failed to parse JSON from " + filepath_);
        return false;
    }
    return true;
}

bool FavoritesManager::save() {
    std::ofstream out(filepath_);
    if (!out.is_open()) {
        util::logLine("favorites: could not open file for writing: " + filepath_);
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& game : favorite_games_) {
            nlohmann::json jg;
            to_json(jg, game);
            j.push_back(jg);
        }
        for (const auto& id : legacy_topic_ids_) {
            j.push_back(id);
        }
        out << j.dump(4);
        util::logLine("favorites: saved successfully");
    } catch (...) {
        util::logLine("favorites: failed to save JSON to " + filepath_);
        return false;
    }
    return true;
}

bool FavoritesManager::isFavorite(const std::string& key_or_topic_id) const {
    if (key_or_topic_id.empty()) return false;
    for (const auto& g : favorite_games_) {
        if (getGameKey(g) == key_or_topic_id || g.topic_id == key_or_topic_id || g.magnet == key_or_topic_id) {
            return true;
        }
    }
    return legacy_topic_ids_.count(key_or_topic_id) > 0;
}

bool FavoritesManager::isFavorite(const Game& game) const {
    std::string key = getGameKey(game);
    return isFavorite(key);
}

void FavoritesManager::addFavorite(const Game& game) {
    std::string key = getGameKey(game);
    if (key.empty()) return;
    if (isFavorite(key)) return;

    favorite_games_.push_back(game);
    if (!game.topic_id.empty()) legacy_topic_ids_.erase(game.topic_id);
    save();
}

void FavoritesManager::addFavorite(const std::string& topic_id) {
    if (topic_id.empty()) return;
    auto catalog = getCatalogSnapshot();
    for (const auto& g : *catalog) {
        if (g.topic_id == topic_id || g.magnet == topic_id) {
            addFavorite(g);
            return;
        }
    }
    if (legacy_topic_ids_.insert(topic_id).second) {
        save();
    }
}

void FavoritesManager::removeFavorite(const std::string& key_or_topic_id) {
    if (key_or_topic_id.empty()) return;
    bool erased = false;
    for (auto it = favorite_games_.begin(); it != favorite_games_.end(); ) {
        if (getGameKey(*it) == key_or_topic_id || it->topic_id == key_or_topic_id || it->magnet == key_or_topic_id) {
            it = favorite_games_.erase(it);
            erased = true;
        } else {
            ++it;
        }
    }
    if (legacy_topic_ids_.erase(key_or_topic_id) > 0) {
        erased = true;
    }
    if (erased) {
        save();
    }
}

void FavoritesManager::removeFavorite(const Game& game) {
    std::string key = getGameKey(game);
    removeFavorite(key);
}

bool FavoritesManager::toggleFavorite(const Game& game) {
    if (isFavorite(game)) {
        removeFavorite(game);
        return false;
    } else {
        addFavorite(game);
        return true;
    }
}

bool FavoritesManager::toggleFavorite(const std::string& topic_id) {
    if (isFavorite(topic_id)) {
        removeFavorite(topic_id);
        return false;
    } else {
        addFavorite(topic_id);
        return true;
    }
}

void FavoritesManager::syncLegacyFavorites(const std::vector<Game>& catalog_games) {
    if (legacy_topic_ids_.empty() || catalog_games.empty()) return;

    bool updated = false;
    for (const auto& g : catalog_games) {
        std::string key = getGameKey(g);
        if (legacy_topic_ids_.count(key) > 0 || (!g.topic_id.empty() && legacy_topic_ids_.count(g.topic_id) > 0)) {
            if (!isFavorite(g)) {
                favorite_games_.push_back(g);
                updated = true;
            }
            legacy_topic_ids_.erase(key);
            if (!g.topic_id.empty()) legacy_topic_ids_.erase(g.topic_id);
        }
    }
    if (updated) {
        save();
    }
}

} // namespace catalog
