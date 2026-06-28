#include "FavoritesManager.hpp"
#include <fstream>
#include <borealis/extern/nlohmann/json.hpp>
#include "../utils/log.h"

namespace catalog {

void FavoritesManager::init(const std::string& path) {
    filepath_ = path;
    load();
}

bool FavoritesManager::load() {
    favorites_.clear();
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
                if (item.is_string()) {
                    favorites_.insert(item.get<std::string>());
                }
            }
        }
        util::logLine("favorites: loaded count=" + std::to_string(favorites_.size()));
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
        for (const auto& id : favorites_) {
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

bool FavoritesManager::isFavorite(const std::string& topic_id) const {
    if (topic_id.empty()) return false;
    return favorites_.count(topic_id) > 0;
}

void FavoritesManager::addFavorite(const std::string& topic_id) {
    if (topic_id.empty()) return;
    if (favorites_.insert(topic_id).second) {
        save();
    }
}

void FavoritesManager::removeFavorite(const std::string& topic_id) {
    if (topic_id.empty()) return;
    if (favorites_.erase(topic_id) > 0) {
        save();
    }
}

bool FavoritesManager::toggleFavorite(const std::string& topic_id) {
    if (topic_id.empty()) return false;
    if (isFavorite(topic_id)) {
        removeFavorite(topic_id);
        return false;
    } else {
        addFavorite(topic_id);
        return true;
    }
}

} // namespace catalog
