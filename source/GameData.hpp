#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <thread>
#include <filesystem>
#include <sys/stat.h>
#include <borealis.hpp>
#include <borealis/core/cache_helper.hpp>
#include "net/http_client.h"
#include "utils/log.h"
#include <borealis/extern/nlohmann/json.hpp>

struct Game {
    std::string title;
    std::string size;
    std::string magnet;
    std::string topic_id;
    std::string url;
    std::string year;
    std::string genre;
    std::string developer;
    std::string publisher;
    std::string image_format;
    std::string interface_lang;
    std::string voice_lang;
    std::string cover;
    std::vector<std::string> screenshots;
    std::string description;
};

inline std::string safeGetStr(const nlohmann::json& j, const std::string& key) {
    if (!j.contains(key) || j[key].is_null()) return "";
    try {
        if (j[key].is_string()) return j[key].get<std::string>();
        if (j[key].is_number()) return std::to_string(j[key].get<double>());
        if (j[key].is_boolean()) return j[key].get<bool>() ? "true" : "false";
    } catch (...) {}
    return "";
}

// nlohmann::json deserialization
inline void from_json(const nlohmann::json& j, Game& g) {
    g.title = safeGetStr(j, "title");
    g.size = safeGetStr(j, "size");
    g.magnet = safeGetStr(j, "magnet");
    g.topic_id = safeGetStr(j, "topic_id");
    g.url = safeGetStr(j, "url");
    g.year = safeGetStr(j, "year");
    g.genre = safeGetStr(j, "genre");
    g.developer = safeGetStr(j, "developer");
    g.publisher = safeGetStr(j, "publisher");
    g.image_format = safeGetStr(j, "image_format");
    g.interface_lang = safeGetStr(j, "interface_lang");
    g.voice_lang = safeGetStr(j, "voice_lang");
    g.cover = safeGetStr(j, "cover");
    
    g.screenshots.clear();
    if (j.contains("screenshots") && j["screenshots"].is_array()) {
        for (const auto& item : j["screenshots"]) {
            if (item.is_string()) {
                g.screenshots.push_back(item.get<std::string>());
            }
        }
    }
    
    g.description = safeGetStr(j, "description");
}

// Clean title: removes everything from the first '[' to the end of string, and trims trailing spaces
inline std::string cleanTitle(const std::string& title) {
    size_t pos = title.find('[');
    if (pos == std::string::npos) return title;
    std::string cleaned = title.substr(0, pos);
    while (!cleaned.empty() && std::isspace(static_cast<unsigned char>(cleaned.back()))) {
        cleaned.pop_back();
    }
    return cleaned;
}

// Truncate a UTF-8 string to at most maxCodepoints Unicode codepoints,
// appending an ellipsis if the string was longer.
// This is used for catalog card titles to guarantee text never overflows
// the fixed-width 150px label box regardless of Yoga layout quirks.
inline std::string truncateCatalogTitle(const std::string& s, size_t maxCodepoints = 16) {
    size_t count = 0;
    size_t i = 0;
    while (i < s.size() && count < maxCodepoints) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if      (c < 0x80)  i += 1;
        else if (c < 0xE0)  i += 2;
        else if (c < 0xF0)  i += 3;
        else                 i += 4;
        ++count;
    }
    if (i >= s.size()) return s;          // fits — return as-is
    // Find byte boundary to cut cleanly
    return s.substr(0, i) + "\u2026";    // U+2026 HORIZONTAL ELLIPSIS
}

// Extract language badge: gets the portion inside the brackets of interface_lang (e.g. "[RUS / ENG]" -> "RUS / ENG")
inline std::string extractLangBadge(const std::string& interface_lang) {
    size_t start = interface_lang.find('[');
    if (start == std::string::npos) return "";
    size_t end = interface_lang.find(']', start);
    if (end == std::string::npos) return interface_lang.substr(start + 1);
    return interface_lang.substr(start + 1, end - start - 1);
}

// Load games from JSON file
inline std::vector<Game> loadGamesFromFile(const std::string& path) {
    util::logLine("GameData: loading games from " + path);
    std::vector<Game> games;
    std::ifstream in(path);
    if (!in.is_open()) {
        util::logLine("GameData: failed to open file " + path);
        return games;
    }
    try {
        nlohmann::json j;
        in >> j;
        if (j.is_array()) {
            games = j.get<std::vector<Game>>();
            util::logLine("GameData: loaded array, count=" + std::to_string(games.size()));
        } else if (j.is_object()) {
            if (j.contains("games") && j["games"].is_array()) {
                games = j["games"].get<std::vector<Game>>();
                util::logLine("GameData: loaded object.games, count=" + std::to_string(games.size()));
            } else if (j.contains("entries") && j["entries"].is_array()) {
                games = j["entries"].get<std::vector<Game>>();
                util::logLine("GameData: loaded object.entries, count=" + std::to_string(games.size()));
            } else {
                util::logLine("GameData: JSON object does not contain 'games' or 'entries' array");
            }
        } else {
            util::logLine("GameData: JSON root is neither array nor object");
        }
    } catch (const std::exception& e) {
        util::logLine(std::string("GameData: JSON parsing exception: ") + e.what());
    } catch (...) {
        util::logLine("GameData: JSON parsing unknown exception");
    }
    return games;
}

#include <unordered_set>
#include <dirent.h>

inline std::unordered_set<std::string> g_cached_images;

inline void cacheImagesInit() {
    g_cached_images.clear();
    struct stat st;
    if (stat("sdmc:/switch/TorrentShopNX", &st) != 0) {
        mkdir("sdmc:/switch/TorrentShopNX", 0777);
    }
    if (stat("sdmc:/switch/TorrentShopNX/cache", &st) != 0) {
        mkdir("sdmc:/switch/TorrentShopNX/cache", 0777);
    }

    DIR* dir = opendir("sdmc:/switch/TorrentShopNX/cache");
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) {
            g_cached_images.insert(entry->d_name);
        }
    }
    closedir(dir);
    util::logLine("GameData: cached images count=" + std::to_string(g_cached_images.size()));
}

// Asynchronously download and cache images from URLs, showing placeholder during download
inline void setImageFromHTTPS(brls::Image* img, const std::string& url, std::shared_ptr<bool> token = nullptr, const std::string& placeholder = "romfs:/img/borealis_96.png") {
    if (url.empty() || !img) {
        if (img) img->setImageFromFile(placeholder);
        return;
    }
    
    std::string safeName = url;
    for (char& c : safeName) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    std::string fileName = safeName + ".png";
    std::string cachePath = "sdmc:/switch/TorrentShopNX/cache/" + fileName;
    
    // 1. If it's already in the GPU/RAM texture cache, use it immediately (super fast, no file I/O)
    if (brls::TextureCache::instance().getCache(cachePath) > 0) {
        img->setImageFromFile(cachePath);
        return;
    }
    
    // 2. If it's cached on the SD card, read it asynchronously on a background thread
    if (g_cached_images.count(fileName) > 0) {
        img->setImageFromFile(placeholder); // Show placeholder while reading
        
        brls::async([img, cachePath, token]() {
            std::ifstream in(cachePath, std::ios::binary | std::ios::ate);
            if (!in.is_open()) return;
            
            std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);
            std::vector<unsigned char> buffer(size);
            if (in.read(reinterpret_cast<char*>(buffer.data()), size)) {
                in.close();
                
                brls::sync([img, cachePath, buffer = std::move(buffer), token]() {
                    if (token && !*token) return;
                    
                    if (brls::TextureCache::instance().getCache(cachePath) == 0) {
                        int tex = nvgCreateImageMem(brls::Application::getNVGContext(), 0, const_cast<unsigned char*>(buffer.data()), buffer.size());
                        if (tex > 0) {
                            brls::TextureCache::instance().addCache(cachePath, tex);
                        }
                    }
                    img->setImageFromFile(cachePath);
                });
            }
        });
        return;
    }
    
    // 3. Otherwise, download it from the network asynchronously
    img->setImageFromFile(placeholder);
    
    brls::async([img, url, cachePath, fileName, token]() {
        net::HttpClient http;
        auto res = http.httpGet(url);
        if (res.status_code == 200 && !res.body.empty()) {
            // Save to SD card for future runs
            std::ofstream out(cachePath, std::ios::binary);
            if (out.is_open()) {
                out.write(res.body.data(), res.body.size());
                out.close();
            }
            
            // Upload to GPU directly from the downloaded buffer (avoiding SD card read)
            brls::sync([img, cachePath, fileName, body = std::move(res.body), token]() {
                g_cached_images.insert(fileName);
                if (token && !*token) return;
                
                if (brls::TextureCache::instance().getCache(cachePath) == 0) {
                    int tex = nvgCreateImageMem(
                        brls::Application::getNVGContext(), 
                        0, 
                        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(body.data())), 
                        body.size()
                    );
                    if (tex > 0) {
                        brls::TextureCache::instance().addCache(cachePath, tex);
                    }
                }
                img->setImageFromFile(cachePath);
            });
        }
    });
}
