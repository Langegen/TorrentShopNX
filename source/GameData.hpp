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
#include "net/image_downloader.h"
#include "config/config.h"
#include "utils/log.h"
#include <borealis/extern/nlohmann/json.hpp>

struct Game {
    std::string title;
    std::string title_id;
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
    g.title_id = safeGetStr(j, "title_id");
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

// nlohmann::json serialization
inline void to_json(nlohmann::json& j, const Game& g) {
    j = nlohmann::json{
        {"title", g.title},
        {"title_id", g.title_id},
        {"size", g.size},
        {"magnet", g.magnet},
        {"topic_id", g.topic_id},
        {"url", g.url},
        {"year", g.year},
        {"genre", g.genre},
        {"developer", g.developer},
        {"publisher", g.publisher},
        {"image_format", g.image_format},
        {"interface_lang", g.interface_lang},
        {"voice_lang", g.voice_lang},
        {"cover", g.cover},
        {"screenshots", g.screenshots},
        {"description", g.description}
    };
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

// Fast file reading into a preallocated std::string buffer
inline bool readFileFast(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        in.read(&out[0], size);
        if (!in && in.gcount() != size) {
            out.resize(static_cast<size_t>(in.gcount()));
        }
    }
    return true;
}

// Parse games directly from a JSON string in memory
inline std::vector<Game> parseGamesFromJsonString(const std::string& jsonContent) {
    util::logLine("GameData: parsing games from JSON string (size=" + std::to_string(jsonContent.size()) + ")");
    std::vector<Game> games;
    if (jsonContent.empty()) return games;
    try {
        nlohmann::json j = nlohmann::json::parse(jsonContent);
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

// Save games vector to high-speed binary cache
inline bool saveGamesToBinaryFile(const std::string& binPath, const std::vector<Game>& games) {
    if (games.empty()) return false;
    std::filesystem::path p(binPath);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ofstream out(binPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        util::logLine("GameData: failed to open bin file for writing: " + binPath);
        return false;
    }

    const char magic[8] = {'T', 'S', 'N', 'X', 'B', 'I', 'N', '2'};
    out.write(magic, 8);
    uint32_t version = 1;
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    uint32_t count = static_cast<uint32_t>(games.size());
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));

    auto writeStr = [&out](const std::string& s) {
        uint16_t len = static_cast<uint16_t>(std::min<size_t>(s.size(), 65535));
        out.write(reinterpret_cast<const char*>(&len), sizeof(len));
        if (len > 0) {
            out.write(s.data(), len);
        }
    };

    for (const auto& g : games) {
        writeStr(g.title);
        writeStr(g.title_id);
        writeStr(g.size);
        writeStr(g.magnet);
        writeStr(g.topic_id);
        writeStr(g.url);
        writeStr(g.year);
        writeStr(g.genre);
        writeStr(g.developer);
        writeStr(g.publisher);
        writeStr(g.image_format);
        writeStr(g.interface_lang);
        writeStr(g.voice_lang);
        writeStr(g.cover);

        uint16_t scCount = static_cast<uint16_t>(std::min<size_t>(g.screenshots.size(), 65535));
        out.write(reinterpret_cast<const char*>(&scCount), sizeof(scCount));
        for (size_t i = 0; i < scCount; ++i) {
            writeStr(g.screenshots[i]);
        }

        writeStr(g.description);
    }

    out.flush();
    bool ok = out.good();
    util::logLine("GameData: saved binary cache to " + binPath + " count=" + std::to_string(games.size()) + " ok=" + std::to_string(ok));
    return ok;
}

// Load games vector from high-speed binary cache (<50ms for 7000+ games)
inline bool loadGamesFromBinaryFile(const std::string& binPath, std::vector<Game>& games) {
    std::string buffer;
    if (!readFileFast(binPath, buffer)) {
        return false;
    }
    if (buffer.size() < 16) {
        util::logLine("GameData: bin cache file too small (" + std::to_string(buffer.size()) + " bytes)");
        return false;
    }

    const char* ptr = buffer.data();
    const char* end = buffer.data() + buffer.size();

    const char magic[8] = {'T', 'S', 'N', 'X', 'B', 'I', 'N', '2'};
    if (std::memcmp(ptr, magic, 8) != 0) {
        util::logLine("GameData: bin cache magic mismatch");
        return false;
    }
    ptr += 8;

    uint32_t version = 0;
    std::memcpy(&version, ptr, sizeof(version));
    ptr += sizeof(version);
    if (version != 1) {
        util::logLine("GameData: bin cache unsupported version " + std::to_string(version));
        return false;
    }

    uint32_t count = 0;
    std::memcpy(&count, ptr, sizeof(count));
    ptr += sizeof(count);

    auto readStr = [&ptr, end](std::string& s) -> bool {
        if (ptr + sizeof(uint16_t) > end) return false;
        uint16_t len = 0;
        std::memcpy(&len, ptr, sizeof(len));
        ptr += sizeof(len);
        if (ptr + len > end) return false;
        s.assign(ptr, len);
        ptr += len;
        return true;
    };

    games.clear();
    games.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        Game g;
        if (!readStr(g.title)) return false;
        if (!readStr(g.title_id)) return false;
        if (!readStr(g.size)) return false;
        if (!readStr(g.magnet)) return false;
        if (!readStr(g.topic_id)) return false;
        if (!readStr(g.url)) return false;
        if (!readStr(g.year)) return false;
        if (!readStr(g.genre)) return false;
        if (!readStr(g.developer)) return false;
        if (!readStr(g.publisher)) return false;
        if (!readStr(g.image_format)) return false;
        if (!readStr(g.interface_lang)) return false;
        if (!readStr(g.voice_lang)) return false;
        if (!readStr(g.cover)) return false;

        if (ptr + sizeof(uint16_t) > end) return false;
        uint16_t scCount = 0;
        std::memcpy(&scCount, ptr, sizeof(scCount));
        ptr += sizeof(scCount);

        g.screenshots.reserve(scCount);
        for (uint16_t s = 0; s < scCount; ++s) {
            std::string sc;
            if (!readStr(sc)) return false;
            g.screenshots.push_back(std::move(sc));
        }

        if (!readStr(g.description)) return false;

        games.push_back(std::move(g));
    }

    util::logLine("GameData: loaded " + std::to_string(games.size()) + " games from binary cache " + binPath);
    return true;
}

// Load games from JSON file using fast block I/O
inline std::vector<Game> loadGamesFromFile(const std::string& path) {
    util::logLine("GameData: loading games from " + path);
    std::string content;
    if (!readFileFast(path, content)) {
        util::logLine("GameData: failed to open file " + path);
        return {};
    }
    return parseGamesFromJsonString(content);
}

#include <unordered_set>
#include <dirent.h>
#include <atomic>

inline std::string getCatalogPath() {
    auto& cfg = config::ConfigManager::instance();
    std::string effUrl = cfg.getEffectiveCatalogSourceUrl();
    if (effUrl.find("EN_catalog") != std::string::npos) {
        return "sdmc:/switch/TorrentShopNX/switch_games_en.json";
    }
    return "sdmc:/switch/TorrentShopNX/switch_games_ru.json";
}

inline std::string getCatalogBinPath() {
    auto& cfg = config::ConfigManager::instance();
    std::string effUrl = cfg.getEffectiveCatalogSourceUrl();
    if (effUrl.find("EN_catalog") != std::string::npos) {
        return "sdmc:/switch/TorrentShopNX/switch_games_en.bin";
    }
    return "sdmc:/switch/TorrentShopNX/switch_games_ru.bin";
}

inline const char* kCatalogPath = "sdmc:/switch/TorrentShopNX/switch_games_ru.json";
inline const char* kCatalogBinPath = "sdmc:/switch/TorrentShopNX/switch_games_ru.bin";

// Cached loader: prefers instant binary cache if up-to-date, falls back to JSON + generates binary cache
inline std::vector<Game> loadGamesCached(const std::string& jsonPath, const std::string& binPath) {
    struct stat stBin, stJson;
    bool hasBin = (stat(binPath.c_str(), &stBin) == 0);
    bool hasJson = (stat(jsonPath.c_str(), &stJson) == 0);

    // Fallback to legacy switch_games.bin / switch_games.json for Russian catalog if new lang files don't exist yet
    if (!hasBin && !hasJson && binPath.find("switch_games_ru.bin") != std::string::npos) {
        std::string legacyBin = "sdmc:/switch/TorrentShopNX/switch_games.bin";
        std::string legacyJson = "sdmc:/switch/TorrentShopNX/switch_games.json";
        if (stat(legacyBin.c_str(), &stBin) == 0) {
            std::vector<Game> games;
            if (loadGamesFromBinaryFile(legacyBin, games) && !games.empty()) {
                return games;
            }
        }
        if (stat(legacyJson.c_str(), &stJson) == 0) {
            std::vector<Game> games = loadGamesFromFile(legacyJson);
            if (!games.empty()) {
                saveGamesToBinaryFile(binPath, games);
                return games;
            }
        }
    }

    if (hasBin && (!hasJson || stBin.st_mtime >= stJson.st_mtime)) {
        std::vector<Game> games;
        if (loadGamesFromBinaryFile(binPath, games) && !games.empty()) {
            return games;
        }
        util::logLine("GameData: binary cache invalid or empty, falling back to JSON");
    }

    if (hasJson) {
        std::vector<Game> games = loadGamesFromFile(jsonPath);
        if (!games.empty()) {
            saveGamesToBinaryFile(binPath, games);
        }
        return games;
    }

    return {};
}
inline std::vector<std::string> g_pathsToDelete;
inline std::atomic<bool> g_appExiting{false};
inline std::atomic<bool> g_cleanupCancelled{false};
inline std::atomic<bool> g_cleanupRunning{false};
inline std::atomic<bool> g_catalogUpdateRunning{false};

inline void deleteDirectoryIterative(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;

    try {
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (g_cleanupCancelled.load()) return;
            paths.push_back(entry.path());
        }

        // Delete files first, then directories (reverse order)
        std::reverse(paths.begin(), paths.end());
        for (const auto& p : paths) {
            if (g_cleanupCancelled.load()) return;
            std::filesystem::remove(p, ec);
        }
        // Finally, delete the root folder
        std::filesystem::remove(path, ec);
    } catch (...) {
        if (!g_cleanupCancelled.load()) {
            std::filesystem::remove_all(path, ec);
        }
    }
}

inline bool writeTextFile(const std::string& path, const std::string& body) {
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(body.data(), body.size());
    return true;
}

inline std::string extractBtihHashLocal(std::string magnet) {
    std::transform(magnet.begin(), magnet.end(), magnet.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::string marker = "xt=urn:btih:";
    size_t pos = magnet.find(marker);
    if (pos == std::string::npos) return {};
    pos += marker.size();
    size_t end = magnet.find('&', pos);
    if (end == std::string::npos) end = magnet.size();
    if (end <= pos) return {};
    return magnet.substr(pos, end - pos);
}

inline void clearCaches() {
    struct stat st;
    if (stat("sdmc:/switch/TorrentShopNX", &st) != 0) {
        mkdir("sdmc:/switch/TorrentShopNX", 0777);
    }

    std::error_code ec;
#ifndef __SWITCH__
    std::filesystem::path localEngineCache = "./cache/local_engine";
    std::filesystem::path tempDeletePath = "./cache/local_engine_old";
#else
    std::filesystem::path localEngineCache = "sdmc:/switch/TorrentShopNX/cache/local_engine";
    std::filesystem::path tempDeletePath = "sdmc:/switch/TorrentShopNX/cache/local_engine_old";
#endif

    // Safely and instantly rename the directory to prevent any race conditions with TorrentEngine.
    // The directory will be deleted asynchronously later on the main loop.
    if (std::filesystem::exists(localEngineCache, ec)) {
        int suffix = 0;
        std::filesystem::path targetDelete = tempDeletePath;
        while (std::filesystem::exists(targetDelete, ec)) {
            targetDelete = tempDeletePath.string() + "_" + std::to_string(++suffix);
        }
        std::filesystem::rename(localEngineCache, targetDelete, ec);
        if (!ec) {
            g_pathsToDelete.push_back(targetDelete.string());
        } else {
            // Fallback to synchronous delete if rename failed
            std::filesystem::remove_all(localEngineCache, ec);
            util::logLine("GameData: cleared local_engine cache synchronously (rename failed)");
        }
    }

    // Move duplicate TorrentShopNX folder cleanup to queue as well
#ifndef __SWITCH__
    std::filesystem::path duplicateFolder = "./TorrentShopNX";
#else
    std::filesystem::path duplicateFolder = "sdmc:/switch/TorrentShopNX/TorrentShopNX";
#endif
    if (std::filesystem::exists(duplicateFolder, ec)) {
#ifndef __SWITCH__
        std::filesystem::path tempDuplicateDelete = "./TorrentShopNX_old";
#else
        std::filesystem::path tempDuplicateDelete = "sdmc:/switch/TorrentShopNX/TorrentShopNX_old";
#endif
        int suffix = 0;
        std::filesystem::path targetDelete = tempDuplicateDelete;
        while (std::filesystem::exists(targetDelete, ec)) {
            targetDelete = tempDuplicateDelete.string() + "_" + std::to_string(++suffix);
        }
        std::filesystem::rename(duplicateFolder, targetDelete, ec);
        if (!ec) {
            g_pathsToDelete.push_back(targetDelete.string());
        } else {
            std::filesystem::remove_all(duplicateFolder, ec);
            util::logLine("GameData: removed duplicated TorrentShopNX folder synchronously (rename failed)");
        }
    }
}

// Helper to normalize image URLs (e.g. FastPic migrated fastpic.ru to fastpic.org and storage servers fail on HTTPS)
inline std::string normalizeImageUrl(const std::string& url) {
    std::string norm = url;
    if (norm.find("fastpic.") != std::string::npos) {
        if (norm.rfind("https://", 0) == 0) {
            norm.replace(0, 5, "http");
        }
        size_t ruPos = norm.find("fastpic.ru");
        if (ruPos != std::string::npos) {
            norm.replace(ruPos, 10, "fastpic.org");
        }
    }
    if (norm.rfind("https://", 0) == 0 && (norm.find("imagebam.com") != std::string::npos || norm.find("vfl.ru") != std::string::npos)) {
        norm.replace(0, 5, "http");
    }
    return norm;
}

// Helper to convert thumbnail URLs from popular image hostings to their original/full-size versions
inline std::string getOriginalImageUrl(const std::string& url) {
    std::string original = normalizeImageUrl(url);
    
    // FastPic
    size_t fpThumb = original.find("/thumb/");
    if (fpThumb != std::string::npos && (original.find("fastpic.") != std::string::npos)) {
        original.replace(fpThumb, 7, "/big/");
        size_t pos = original.rfind(".jpeg");
        if (pos != std::string::npos && pos == original.length() - 5) {
            original.replace(pos, 5, ".jpg");
        }
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
inline void setImageFromHTTPS(brls::Image* img, const std::string& url, std::shared_ptr<bool> token = nullptr, const std::string& placeholder = "romfs:/img/borealis_96.png", bool bypassCache = false, const std::string& fallbackUrl = "", int row = -1, int col = -1, int priorityOverride = 0) {
    if (url.empty() || !img) {
        if (img) img->setImageFromFile(placeholder);
        return;
    }
    
    std::string normUrl = normalizeImageUrl(url);
    std::string safeName = normUrl;
    for (char& c : safeName) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    std::string fileName = safeName + ".png";
    std::string cacheKey = "memory_cache:/" + fileName;

    std::string effectiveFallback = fallbackUrl;
    if (effectiveFallback.empty()) {
        std::string orig = getOriginalImageUrl(normUrl);
        if (orig != normUrl) {
            effectiveFallback = orig;
        }
    }

    net::ImageDownloader::instance().enqueue(img, normUrl, cacheKey, token, placeholder, bypassCache, effectiveFallback, row, col, priorityOverride);
}

#include <sstream>

inline uint64_t parseTitleIdFromString(const std::string& text) {
    size_t start = text.find('[');
    while (start != std::string::npos) {
        size_t end = text.find(']', start);
        if (end == std::string::npos) break;
        std::string inner = text.substr(start + 1, end - start - 1);
        if (inner.size() == 16) {
            bool isHex = true;
            for (char c : inner) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) {
                    isHex = false;
                    break;
                }
            }
            if (isHex) {
                try {
                    return std::stoull(inner, nullptr, 16);
                } catch (...) {}
            }
        }
        start = text.find('[', end);
    }
    return 0;
}

// Resolve a game's title ID: prefer the explicit `title_id` field from the
// catalog JSON (16-hex or 0x... form), fall back to parsing the [0100...]
// bracket from the title.
inline uint64_t parseTitleIdFromGame(const Game& g) {
    std::string gid = g.title_id;
    if (!gid.empty()) {
        size_t start = 0;
        if (gid.size() >= 2 && gid[0] == '0' && (gid[1] == 'x' || gid[1] == 'X')) start = 2;
        std::string hex = gid.substr(start);
        if (hex.size() == 16) {
            bool isHex = true;
            for (char c : hex) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) { isHex = false; break; }
            }
            if (isHex) {
                try { return std::stoull(hex, nullptr, 16); } catch (...) {}
            }
        }
    }
    return parseTitleIdFromString(g.title);
}

inline std::string parseVersionFromTitle(const std::string& text) {
    size_t start = text.find('[');
    while (start != std::string::npos) {
        size_t end = text.find(']', start);
        if (end == std::string::npos) break;
        std::string inner = text.substr(start + 1, end - start - 1);
        if (!inner.empty() && inner[0] == 'v') {
            return inner.substr(1);
        }
        start = text.find('[', end);
    }
    return "";
}

inline uint32_t convertVersionStringToNumber(const std::string& versionStr) {
    if (versionStr.empty()) return 0;
    
    // Check if it's already a single integer (e.g. "65536")
    bool isNumeric = true;
    for (char c : versionStr) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            isNumeric = false;
            break;
        }
    }
    if (isNumeric) {
        try {
            return static_cast<uint32_t>(std::stoul(versionStr));
        } catch (...) {}
    }
    
    // Parse semver (e.g. "1.3.0")
    std::stringstream ss(versionStr);
    std::string item;
    uint32_t parts[3] = {0, 0, 0};
    int i = 0;
    while (std::getline(ss, item, '.') && i < 3) {
        try {
            parts[i] = static_cast<uint32_t>(std::stoul(item));
        } catch (...) {}
        i++;
    }
    return (parts[0] << 16) | (parts[1] << 8) | parts[2];
}

inline bool compareVersions(const std::string& current, const std::string& available) {
    uint32_t curNum = convertVersionStringToNumber(current);
    uint32_t availNum = convertVersionStringToNumber(available);
    return availNum > curNum;
}

