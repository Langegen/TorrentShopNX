#include "catalog_manager.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <functional>
#include <cstdint>
#include <sys/stat.h>
#include <ctime>
#include <cerrno>

#include "../net/http_client.h"
#include "../rss/rss_parser.h"
#include "../utils/log.h"
#include "../utils/app_paths.h"

namespace catalog {

static std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool writeFile(const std::string& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(body.data(), body.size());
    return true;
}

static bool pathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool ensureDirRecursive(const std::string& path) {
    if (path.empty()) return false;
    if (pathExists(path)) return true;

    std::string cur;
    size_t pos = 0;
    if (path.rfind("sdmc:/", 0) == 0) {
        cur = "sdmc:/";
        pos = 6;
    }

    while (pos < path.size()) {
        size_t next = path.find('/', pos);
        std::string part = (next == std::string::npos) ? path.substr(pos) : path.substr(pos, next - pos);
        if (!part.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += "/";
            cur += part;
            if (!pathExists(cur)) {
                mkdir(cur.c_str(), 0777);
            }
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return pathExists(path);
}

static void appendUtf8(std::string& out, uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

static bool isValidUtf8(const std::string& text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = bytes[i];
        if (c <= 0x7F) {
            ++i;
            continue;
        }

        size_t len = 0;
        uint32_t min_cp = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
            min_cp = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
            min_cp = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
            min_cp = 0x10000;
        } else {
            return false;
        }

        if ((i + len) > text.size()) return false;
        for (size_t j = 1; j < len; ++j) {
            const unsigned char cc = bytes[i + j];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }

        if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += len;
    }
    return true;
}

static bool looksLikeUtf16Le(const std::string& text) {
    if (text.size() < 4 || (text.size() % 2) != 0) return false;
    size_t zero_hi = 0;
    size_t sample = 0;
    for (size_t i = 1; i < text.size() && sample < 64; i += 2, ++sample) {
        if (text[i] == '\0') ++zero_hi;
    }
    return sample > 0 && zero_hi >= (sample * 3) / 4;
}

static bool looksLikeUtf16Be(const std::string& text) {
    if (text.size() < 4 || (text.size() % 2) != 0) return false;
    size_t zero_lo = 0;
    size_t sample = 0;
    for (size_t i = 0; i < text.size() && sample < 64; i += 2, ++sample) {
        if (text[i] == '\0') ++zero_lo;
    }
    return sample > 0 && zero_lo >= (sample * 3) / 4;
}

static std::string utf16ToUtf8(const std::string& text, bool little_endian) {
    std::string out;
    size_t i = 0;
    if (text.size() >= 2) {
        const unsigned char b0 = static_cast<unsigned char>(text[0]);
        const unsigned char b1 = static_cast<unsigned char>(text[1]);
        if ((little_endian && b0 == 0xFF && b1 == 0xFE) ||
            (!little_endian && b0 == 0xFE && b1 == 0xFF)) {
            i = 2;
        }
    }

    while ((i + 1) < text.size()) {
        const unsigned char b0 = static_cast<unsigned char>(text[i]);
        const unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
        uint16_t unit = little_endian ? static_cast<uint16_t>(b0 | (b1 << 8))
                                      : static_cast<uint16_t>((b0 << 8) | b1);
        i += 2;

        uint32_t codepoint = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF && (i + 1) < text.size()) {
            const unsigned char c0 = static_cast<unsigned char>(text[i]);
            const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            uint16_t unit2 = little_endian ? static_cast<uint16_t>(c0 | (c1 << 8))
                                           : static_cast<uint16_t>((c0 << 8) | c1);
            if (unit2 >= 0xDC00 && unit2 <= 0xDFFF) {
                codepoint = 0x10000 + (((unit - 0xD800) << 10) | (unit2 - 0xDC00));
                i += 2;
            }
        }

        appendUtf8(out, codepoint);
    }
    return out;
}

static std::string cp1251ToUtf8(const std::string& text) {
    static const uint16_t table[128] = {
        0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
        0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
        0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
        0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
        0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
        0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
        0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
        0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
        0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
        0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
        0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
        0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
        0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
        0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
        0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F
    };

    std::string out;
    out.reserve(text.size() * 2);
    for (unsigned char c : text) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            appendUtf8(out, table[c - 0x80]);
        }
    }
    return out;
}

static std::string normalizeTextEncoding(const std::string& body) {
    if (body.empty()) return {};

    if (body.size() >= 3 &&
        static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF) {
        return body.substr(3);
    }

    if (body.size() >= 2 &&
        static_cast<unsigned char>(body[0]) == 0xFF &&
        static_cast<unsigned char>(body[1]) == 0xFE) {
        return utf16ToUtf8(body, true);
    }

    if (body.size() >= 2 &&
        static_cast<unsigned char>(body[0]) == 0xFE &&
        static_cast<unsigned char>(body[1]) == 0xFF) {
        return utf16ToUtf8(body, false);
    }

    if (looksLikeUtf16Le(body)) return utf16ToUtf8(body, true);
    if (looksLikeUtf16Be(body)) return utf16ToUtf8(body, false);
    if (isValidUtf8(body)) return body;
    return cp1251ToUtf8(body);
}

static int hexDigitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool parseHex4(const std::string& text, size_t pos, uint16_t& out) {
    if ((pos + 4) > text.size()) return false;
    uint16_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        const int digit = hexDigitValue(text[pos + i]);
        if (digit < 0) return false;
        value = static_cast<uint16_t>((value << 4) | digit);
    }
    out = value;
    return true;
}

static std::string decodeJsonString(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());

    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }

        if ((i + 1) >= raw.size()) {
            out.push_back('\\');
            break;
        }

        const char esc = raw[++i];
        switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint16_t code = 0;
                if (!parseHex4(raw, i + 1, code)) {
                    out += "\\u";
                    break;
                }
                i += 4;

                uint32_t codepoint = code;
                if (code >= 0xD800 && code <= 0xDBFF &&
                    (i + 6) < raw.size() &&
                    raw[i + 1] == '\\' && raw[i + 2] == 'u') {
                    uint16_t code2 = 0;
                    if (parseHex4(raw, i + 3, code2) && code2 >= 0xDC00 && code2 <= 0xDFFF) {
                        codepoint = 0x10000 + (((code - 0xD800) << 10) | (code2 - 0xDC00));
                        i += 6;
                    }
                }

                appendUtf8(out, codepoint);
                break;
            }
            default:
                out.push_back(esc);
                break;
        }
    }

    return normalizeTextEncoding(out);
}

std::string CatalogManager::toLower(const std::string& s) const {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return std::tolower(c); });
    return out;
}

std::string CatalogManager::cachePathFor(const CatalogSource& s) const {
    std::hash<std::string> h;
    size_t v = h(s.type + ":" + s.url);
    std::ostringstream ss;
    ss << TSNX_CACHE_CATALOG << "/" << std::hex << v << ".cache";
    return ss.str();
}

bool CatalogManager::ensureCacheDir() const {
    return ensureDirRecursive(TSNX_CACHE_CATALOG);
}

bool CatalogManager::loadCachedBody(const CatalogSource& s, std::string& out_body) const {
    std::string path = cachePathFor(s);
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    std::time_t now = std::time(nullptr);
    if (now <= 0) return false;
    if ((now - st.st_mtime) > cache_ttl_seconds_) return false;
    out_body = readFile(path);
    return !out_body.empty();
}

void CatalogManager::saveCache(const CatalogSource& s, const std::string& body) const {
    if (body.empty()) return;
    if (!ensureCacheDir()) return;
    std::string path = cachePathFor(s);
    writeFile(path, body);
}

std::string CatalogManager::extractJsonValue(const std::string& obj, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto kpos = obj.find(needle);
    if (kpos == std::string::npos) return "";
    auto colon = obj.find(':', kpos + needle.size());
    if (colon == std::string::npos) return "";

    size_t start = colon + 1;
    while (start < obj.size() && std::isspace(static_cast<unsigned char>(obj[start]))) {
        ++start;
    }
    if (start >= obj.size() || obj[start] != '"') return "";

    size_t end = start + 1;
    bool escaped = false;
    while (end < obj.size()) {
        const char ch = obj[end];
        if (!escaped && ch == '"') break;
        if (!escaped && ch == '\\') {
            escaped = true;
        } else {
            escaped = false;
        }
        ++end;
    }
    if (end >= obj.size()) return "";

    return decodeJsonString(obj.substr(start + 1, end - start - 1));
}

CatalogEntry CatalogManager::parseEntryObject(const std::string& obj) {
    CatalogEntry e;
    e.title = extractJsonValue(obj, "title");
    e.size = extractJsonValue(obj, "size");
    e.magnet = extractJsonValue(obj, "magnet");
    e.category = extractJsonValue(obj, "category");
    e.description = extractJsonValue(obj, "description");
    e.icon = extractJsonValue(obj, "icon");
    return e;
}

std::vector<CatalogEntry> CatalogManager::parseJsonCatalog(const std::string& body) {
    std::vector<CatalogEntry> entries;
    const std::string normalized_body = normalizeTextEncoding(body);
    std::string::size_type pos = 0;
    while (true) {
        auto obj_start = normalized_body.find('{', pos);
        if (obj_start == std::string::npos) break;
        auto obj_end = normalized_body.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        std::string obj = normalized_body.substr(obj_start, obj_end - obj_start + 1);
        CatalogEntry e = parseEntryObject(obj);
        if (!e.title.empty() || !e.magnet.empty()) {
            entries.push_back(e);
        }
        pos = obj_end + 1;
    }
    return entries;
}

std::vector<CatalogSource> CatalogManager::parseSourcesJson(const std::string& body) {
    std::vector<CatalogSource> sources;
    const std::string normalized_body = normalizeTextEncoding(body);
    std::string::size_type pos = 0;
    while (true) {
        auto obj_start = normalized_body.find('{', pos);
        if (obj_start == std::string::npos) break;
        auto obj_end = normalized_body.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        std::string obj = normalized_body.substr(obj_start, obj_end - obj_start + 1);
        CatalogSource s;
        s.name = extractJsonValue(obj, "name");
        s.type = extractJsonValue(obj, "type");
        s.url = extractJsonValue(obj, "url");
        if (!s.type.empty() && !s.url.empty()) {
            sources.push_back(s);
        }
        pos = obj_end + 1;
    }
    return sources;
}

bool CatalogManager::loadCatalogFromFile(const std::string& catalog_path) {
    std::string body = readFile(catalog_path);
    if (body.empty()) {
        return false;
    }

    merged_entries_ = parseJsonCatalog(body);
    sources_.clear();
    CatalogSource s;
    s.name = "switch_games";
    s.type = "json";
    s.url = catalog_path;
    sources_.push_back(s);
    return !merged_entries_.empty();
}

bool CatalogManager::loadSources(const std::string& sources_path) {
    std::string body = readFile(sources_path);
    if (body.empty()) return false;
    sources_ = parseSourcesJson(body);
    return !sources_.empty();
}

bool CatalogManager::loadSourcesWithFallback(const std::string& primary_path, const std::string& fallback_path) {
    if (loadSources(primary_path)) return true;
    return loadSources(fallback_path);
}

void CatalogManager::updateCatalogs() {
    merged_entries_.clear();
    net::HttpClient http;
    rss::RssParser rssp;

    for (const auto& s : sources_) {
        if (s.type == "json") {
            std::string body;
            if (!loadCachedBody(s, body)) {
                auto res = http.httpGet(s.url);
                if (res.status_code == 200) {
                    body = res.body;
                    saveCache(s, body);
                } else {
                    util::logLine("catalog: fetch failed url=" + s.url +
                                  " status=" + std::to_string(res.status_code));
                }
            }
            if (!body.empty()) {
                auto entries = parseJsonCatalog(body);
                merged_entries_.insert(merged_entries_.end(), entries.begin(), entries.end());
            }
        } else if (s.type == "rss") {
            std::string body;
            if (!loadCachedBody(s, body)) {
                auto res = http.httpGet(s.url);
                if (res.status_code == 200) {
                    body = res.body;
                    saveCache(s, body);
                }
            }
            if (!body.empty()) {
                auto items = rssp.parse(body);
                for (const auto& it : items) {
                    CatalogEntry e;
                    e.title = it.title;
                    e.magnet = it.magnet;
                    e.size = it.size;
                    e.category = "rss";
                    merged_entries_.push_back(e);
                }
            }
        } else if (s.type == "magnet") {
            CatalogEntry e;
            e.title = s.name;
            e.magnet = s.url;
            e.category = "magnet";
            merged_entries_.push_back(e);
        } else if (s.type == "torrent") {
            CatalogEntry e;
            e.title = s.name;
            e.magnet = s.url;
            e.category = "torrent";
            merged_entries_.push_back(e);
        }
    }
}

void CatalogManager::mergeCatalogEntries() {
    // Placeholder for future deduplication or source priority rules.
}

std::vector<CatalogEntry> CatalogManager::searchCatalog(const std::string& query) const {
    std::vector<CatalogEntry> results;
    std::string q = toLower(query);
    for (const auto& e : merged_entries_) {
        std::string t = toLower(e.title + " " + e.category + " " + e.description);
        if (t.find(q) != std::string::npos) {
            results.push_back(e);
        }
    }
    return results;
}

} // namespace catalog
