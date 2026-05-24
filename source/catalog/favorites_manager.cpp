#include "favorites_manager.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace catalog {

static std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

static std::string extractString(const std::string& obj, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto kpos = obj.find(needle);
    if (kpos == std::string::npos) {
        needle = "\"" + key + "\" :";
        kpos = obj.find(needle);
        if (kpos == std::string::npos) return "";
    }
    auto start = obj.find('"', kpos + needle.size());
    if (start == std::string::npos) return "";
    
    size_t end = start + 1;
    bool escaped = false;
    while (end < obj.size()) {
        const char ch = obj[end];
        if (!escaped && ch == '"') break;
        if (!escaped && ch == '\\') escaped = true;
        else escaped = false;
        ++end;
    }
    
    if (end >= obj.size()) return "";
    
    std::string res = obj.substr(start + 1, end - start - 1);
    
    // Very basic unescape (enough for our titles/magnets)
    std::string unescaped;
    for (size_t i = 0; i < res.size(); ++i) {
        if (res[i] == '\\' && i + 1 < res.size()) {
            char next = res[++i];
            if (next == 'n') unescaped += '\n';
            else if (next == 't') unescaped += '\t';
            else unescaped += next;
        } else {
            unescaped += res[i];
        }
    }
    return unescaped;
}

void FavoritesManager::init(const std::string& path) {
    filepath_ = path;
    load();
}

bool FavoritesManager::load() {
    favorites_.clear();
    std::ifstream in(filepath_, std::ios::binary);
    if (!in) return false;
    
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string body = ss.str();
    
    std::string::size_type pos = 0;
    while (true) {
        auto obj_start = body.find('{', pos);
        if (obj_start == std::string::npos) break;
        auto obj_end = body.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        
        std::string obj = body.substr(obj_start, obj_end - obj_start + 1);
        CatalogEntry e;
        e.title = extractString(obj, "title");
        e.size = extractString(obj, "size");
        e.magnet = extractString(obj, "magnet");
        e.category = extractString(obj, "category");
        e.description = extractString(obj, "description");
        e.icon = extractString(obj, "icon");
        
        if (!e.title.empty()) {
            favorites_.push_back(e);
        }
        pos = obj_end + 1;
    }
    
    return true;
}

bool FavoritesManager::save() {
    std::ofstream out(filepath_, std::ios::binary);
    if (!out) return false;
    
    out << "[\n";
    for (size_t i = 0; i < favorites_.size(); ++i) {
        const auto& f = favorites_[i];
        out << "  {\n";
        out << "    \"title\": \"" << escapeJson(f.title) << "\",\n";
        out << "    \"size\": \"" << escapeJson(f.size) << "\",\n";
        out << "    \"magnet\": \"" << escapeJson(f.magnet) << "\",\n";
        out << "    \"category\": \"" << escapeJson(f.category) << "\",\n";
        out << "    \"description\": \"" << escapeJson(f.description) << "\",\n";
        out << "    \"icon\": \"" << escapeJson(f.icon) << "\"\n";
        out << "  }";
        if (i + 1 < favorites_.size()) out << ",";
        out << "\n";
    }
    out << "]\n";
    return true;
}

bool FavoritesManager::isFavorite(const std::string& title) const {
    for (const auto& fav : favorites_) {
        if (fav.title == title) return true;
    }
    return false;
}

void FavoritesManager::addFavorite(const CatalogEntry& entry) {
    if (!isFavorite(entry.title)) {
        favorites_.push_back(entry);
        save();
    }
}

void FavoritesManager::removeFavorite(const std::string& title) {
    auto it = std::remove_if(favorites_.begin(), favorites_.end(),
                             [&title](const CatalogEntry& e) { return e.title == title; });
    if (it != favorites_.end()) {
        favorites_.erase(it, favorites_.end());
        save();
    }
}

bool FavoritesManager::toggleFavorite(const CatalogEntry& entry) {
    if (isFavorite(entry.title)) {
        removeFavorite(entry.title);
        return false;
    } else {
        addFavorite(entry);
        return true;
    }
}

} // namespace catalog
