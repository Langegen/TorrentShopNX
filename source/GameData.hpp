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

inline void clearCaches() {
    struct stat st;
    if (stat("sdmc:/switch/TorrentShopNX", &st) != 0) {
        mkdir("sdmc:/switch/TorrentShopNX", 0777);
    }

    std::error_code ec;
    // Clean up entire cache directory to free up space (catalog, local_engine, old images)
    if (std::filesystem::exists("sdmc:/switch/TorrentShopNX/cache", ec)) {
        std::filesystem::remove_all("sdmc:/switch/TorrentShopNX/cache", ec);
        if (!ec) {
            util::logLine("GameData: cleared all caches");
        }
    }

    // Clean up duplicated TorrentShopNX folder caused by a bug in older versions
    if (std::filesystem::exists("sdmc:/switch/TorrentShopNX/TorrentShopNX", ec)) {
        std::filesystem::remove_all("sdmc:/switch/TorrentShopNX/TorrentShopNX", ec);
        if (!ec) {
            util::logLine("GameData: removed duplicated TorrentShopNX folder");
        }
    }
}

// Helper to convert thumbnail URLs from popular image hostings to their original/full-size versions
inline std::string getOriginalImageUrl(const std::string& url) {
    std::string original = url;
    
    // FastPic
    size_t fpThumb = original.find("/thumb/");
    if (fpThumb != std::string::npos && (original.find("fastpic.") != std::string::npos)) {
        original.replace(fpThumb, 7, "/big/");
        return original;
    }
    
    // ImageBan
    size_t ibThumb = original.find("/thumbs/");
    if (ibThumb != std::string::npos && (original.find("imageban.ru") != std::string::npos || original.find("imageban.co") != std::string::npos)) {
        original.replace(ibThumb, 8, "/out/");
        return original;
    }
    
    // ImgBox
    if (original.find("thumbs.imgbox.com") != std::string::npos) {
        size_t hostPos = original.find("thumbs.imgbox.com");
        original.replace(hostPos, 17, "images.imgbox.com");
        size_t tPos = original.rfind("_t.");
        if (tPos != std::string::npos) {
            original.replace(tPos, 3, "_o.");
        }
        return original;
    }

    // LostPic
    size_t lpThumb = original.find("/thumbs/");
    if (lpThumb != std::string::npos && original.find("lostpic.net") != std::string::npos) {
        original.replace(lpThumb, 8, "/orig/");
        return original;
    }

    // PostImages
    if (original.find("thumbs.postimg.cc") != std::string::npos) {
        size_t hostPos = original.find("thumbs.postimg.cc");
        original.replace(hostPos, 17, "i.postimg.cc");
        return original;
    }
    if (original.find("thumbs.postimg.org") != std::string::npos) {
        size_t hostPos = original.find("thumbs.postimg.org");
        original.replace(hostPos, 18, "i.postimg.org");
        return original;
    }

    // PixHost
    if (original.find("pixhost.to/thumbs/") != std::string::npos) {
        size_t tPos = original.find("://t");
        if (tPos != std::string::npos) {
            original.replace(tPos + 3, 1, "img");
        }
        size_t thumbsPos = original.find("/thumbs/");
        if (thumbsPos != std::string::npos) {
            original.replace(thumbsPos, 8, "/images/");
        }
        return original;
    }
    
    return original;
}

// Asynchronously download and cache images from URLs, showing placeholder during download
inline void setImageFromHTTPS(brls::Image* img, const std::string& url, std::shared_ptr<bool> token = nullptr, const std::string& placeholder = "romfs:/img/borealis_96.png", bool bypassCache = false, const std::string& fallbackUrl = "") {
    if (url.empty() || !img) {
        if (img) img->setImageFromFile(placeholder);
        return;
    }
    
    if (bypassCache) {
        img->setImageFromFile(placeholder);
        img->setFreeTexture(true);
        
        brls::async([img, url, token, fallbackUrl, placeholder, bypassCache]() {
            net::HttpClient http;
            auto res = http.httpGet(url);
            if (res.status_code == 200 && !res.body.empty()) {
                brls::sync([img, body = std::move(res.body), token]() {
                    if (token && !*token) return;
                    
                    int tex = nvgCreateImageMem(
                        brls::Application::getNVGContext(), 
                        NVG_IMAGE_GENERATE_MIPMAPS, 
                        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(body.data())), 
                        body.size()
                    );
                    if (tex > 0) {
                        img->innerSetImage(tex);
                    }
                });
            } else if (!fallbackUrl.empty()) {
                brls::sync([img, fallbackUrl, token, placeholder, bypassCache]() {
                    if (token && !*token) return;
                    setImageFromHTTPS(img, fallbackUrl, token, placeholder, bypassCache, "");
                });
            }
        });
        return;
    }
    
    std::string safeName = url;
    for (char& c : safeName) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    std::string fileName = safeName + ".png";
    std::string cacheKey = "memory_cache:/" + fileName;
    
    // 1. If it's already in the GPU/RAM texture cache, use it immediately
    if (brls::TextureCache::instance().getCache(cacheKey) > 0) {
        img->setImageFromFile(cacheKey);
        return;
    }
    
    // 2. Otherwise, download it from the network asynchronously
    img->setImageFromFile(placeholder);
    
    brls::async([img, url, cacheKey, token, fallbackUrl, placeholder, bypassCache]() {
        net::HttpClient http;
        auto res = http.httpGet(url);
        if (res.status_code == 200 && !res.body.empty()) {
            // Upload to GPU directly from the downloaded buffer
            brls::sync([img, cacheKey, body = std::move(res.body), token]() {
                if (token && !*token) return;
                
                if (brls::TextureCache::instance().getCache(cacheKey) == 0) {
                    int tex = nvgCreateImageMem(
                        brls::Application::getNVGContext(), 
                        NVG_IMAGE_GENERATE_MIPMAPS, 
                        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(body.data())), 
                        body.size()
                    );
                    if (tex > 0) {
                        brls::TextureCache::instance().addCache(cacheKey, tex);
                    }
                }
                img->setImageFromFile(cacheKey);
            });
        } else if (!fallbackUrl.empty()) {
            brls::sync([img, fallbackUrl, token, placeholder, bypassCache]() {
                if (token && !*token) return;
                // If primary URL failed, try the fallback URL without fallback to prevent infinite loop
                setImageFromHTTPS(img, fallbackUrl, token, placeholder, bypassCache, "");
            });
        }
    });
}
