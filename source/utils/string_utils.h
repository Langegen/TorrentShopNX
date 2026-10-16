#pragma once
#include <string>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <cctype>

namespace util {
    inline std::string toLowerCopy(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    inline bool equalsIgnoreCase(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        return std::equal(a.begin(), a.end(), b.begin(), [](unsigned char ca, unsigned char cb) {
            return std::tolower(ca) == std::tolower(cb);
        });
    }

    inline std::string extractBtihHash(std::string magnet) {
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

    // UTF-8 lowercase helper handling both ASCII and Cyrillic (CP1251 / UTF-8)
    inline std::string toLowerUtf8(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80) {
                out.push_back(static_cast<char>(std::tolower(c)));
                i += 1;
            } else if (c == 0xD0 && i + 1 < s.size()) {
                unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
                if (c2 == 0x81) { // Ё -> ё (0xD1 0x91)
                    out.push_back(static_cast<char>(0xD1));
                    out.push_back(static_cast<char>(0x91));
                } else if (c2 >= 0x90 && c2 <= 0x9F) { // А..П -> а..п (0xD0 0xB0..0xBF)
                    out.push_back(static_cast<char>(0xD0));
                    out.push_back(static_cast<char>(c2 + 0x20));
                } else if (c2 >= 0xA0 && c2 <= 0xAF) { // Р..Я -> р..я (0xD1 0x80..0x8F)
                    out.push_back(static_cast<char>(0xD1));
                    out.push_back(static_cast<char>(c2 - 0x20));
                } else {
                    out.push_back(s[i]);
                    out.push_back(s[i + 1]);
                }
                i += 2;
            } else if (c == 0xD1 && i + 1 < s.size()) {
                out.push_back(s[i]);
                out.push_back(s[i + 1]);
                i += 2;
            } else {
                out.push_back(s[i]);
                i += 1;
            }
        }
        return out;
    }

    inline std::string toHex(const std::string& binary) {
        static const char* digits = "0123456789abcdef";
        std::string res;
        res.reserve(binary.size() * 2);
        for (unsigned char c : binary) {
            res.push_back(digits[(c >> 4) & 0x0f]);
            res.push_back(digits[c & 0x0f]);
        }
        return res;
    }

    // Compare two version strings ("1.2.3", "2.10" or "v1.2.3").
    // Returns <0 if a is older than b, 0 if equal, >0 if a is newer than b.
    inline int compareSemver(const std::string& a, const std::string& b) {
        auto parseParts = [](const std::string& s) {
            std::vector<int> parts;
            std::string cur;
            for (char c : s) {
                if (c == '.') {
                    parts.push_back(cur.empty() ? 0 : std::atoi(cur.c_str()));
                    cur.clear();
                } else if (c >= '0' && c <= '9') {
                    cur.push_back(c);
                }
            }
            parts.push_back(cur.empty() ? 0 : std::atoi(cur.c_str()));
            while (parts.size() < 3) parts.push_back(0);
            return parts;
        };

        std::vector<int> av = parseParts(a);
        std::vector<int> bv = parseParts(b);
        for (int i = 0; i < 3; ++i) {
            if (av[i] != bv[i]) {
                return av[i] < bv[i] ? -1 : 1;
            }
        }
        return 0;
    }

    struct ComponentClassification {
        int type = 0;              // 0: Base, 1: Update, 2: DLC, 3: Mod/Extra, 4: Other
        std::string label;         // "Игра", "Обновление", "DLC", "Русификатор", "Мод"
        std::string cleanName;     // Clean, human-readable name without repeating the game title or Atmosphere paths
    };

    inline ComponentClassification classifyDownloadComponent(const std::string& raw_name, const std::string& parent_title = "") {
        ComponentClassification res;
        std::string filename = raw_name;
        size_t slash = filename.find_last_of("/\\");
        if (slash != std::string::npos) {
            filename = filename.substr(slash + 1);
        }

        std::string lower = toLowerUtf8(raw_name);

        bool is_package = (lower.size() >= 4 && (lower.rfind(".nsp") == lower.size() - 4 ||
                                                lower.rfind(".nsz") == lower.size() - 4 ||
                                                lower.rfind(".xci") == lower.size() - 4 ||
                                                lower.rfind(".xcz") == lower.size() - 4));

        if (!is_package) {
            res.type = 3;
            if (lower.find("rus") != std::string::npos || lower.find("русиф") != std::string::npos ||
                lower.find("русс") != std::string::npos || lower.find("voice") != std::string::npos ||
                lower.find("sound") != std::string::npos || lower.find("озвуч") != std::string::npos ||
                lower.find("перевод") != std::string::npos) {
                res.label = "Русификатор";
            } else if (lower.find("mod") != std::string::npos || lower.find("atmosphere") != std::string::npos ||
                       lower.find("romfs") != std::string::npos || lower.find("exefs") != std::string::npos) {
                res.label = "Мод";
            } else {
                res.label = "Доп. файлы";
            }

            // Find best human-readable directory or file name
            // e.g. "TMNT/Russian Text + Voice Mod (27.11.2024)/atmosphere/..."
            std::vector<std::string> parts;
            size_t start = 0;
            for (size_t i = 0; i < raw_name.size(); ++i) {
                if (raw_name[i] == '/' || raw_name[i] == '\\') {
                    if (i > start) parts.push_back(raw_name.substr(start, i - start));
                    start = i + 1;
                }
            }
            if (start < raw_name.size()) parts.push_back(raw_name.substr(start));

            std::string chosen;
            for (const auto& p : parts) {
                std::string lp = toLowerUtf8(p);
                if (lp == "atmosphere" || lp == "contents" || lp == "romfs" || lp == "exefs" || lp == "content") continue;
                if (lp.size() == 16 && lp.find("0100") == 0) continue;
                chosen = p;
                if (lp.find("rus") != std::string::npos || lp.find("mod") != std::string::npos || lp.find("voice") != std::string::npos || lp.find("русиф") != std::string::npos) {
                    break;
                }
            }
            if (chosen.empty()) chosen = filename;
            res.cleanName = chosen;
            return res;
        }

        // Package parsing
        // Check for DLC
        size_t dlc_pos = filename.find("[DLC");
        if (dlc_pos == std::string::npos) dlc_pos = filename.find("(DLC");
        if (dlc_pos != std::string::npos) {
            res.type = 2;
            res.label = "DLC";
            size_t close_bracket = filename.find_first_of("])", dlc_pos);
            if (close_bracket != std::string::npos) {
                std::string dlc_str = filename.substr(dlc_pos + 1, close_bracket - dlc_pos - 1);
                if (dlc_str.find("DLC") == 0) {
                    dlc_str = dlc_str.substr(3);
                    while (!dlc_str.empty() && (dlc_str.front() == ' ' || dlc_str.front() == ':' || dlc_str.front() == '-')) {
                        dlc_str.erase(dlc_str.begin());
                    }
                }
                res.cleanName = dlc_str.empty() ? filename : dlc_str;
            } else {
                res.cleanName = filename;
            }
            return res;
        }

        // Check for Update / Patch
        size_t v_pos = filename.find("[v");
        if (v_pos == std::string::npos) v_pos = filename.find("(v");
        if (v_pos != std::string::npos) {
            size_t close_bracket = filename.find_first_of("])", v_pos);
            std::string v_str = (close_bracket != std::string::npos) ? filename.substr(v_pos + 1, close_bracket - v_pos - 1) : "";
            if (v_str != "v0" && v_str != "v0.0") {
                res.type = 1;
                res.label = "Обновление";
                res.cleanName = "Патч " + v_str;
                return res;
            }
        }

        // Check title ID if available
        size_t tid_pos = filename.find("[0100");
        if (tid_pos != std::string::npos && tid_pos + 20 < filename.size()) {
            std::string tid_str = filename.substr(tid_pos + 1, 16);
            if (tid_str.size() == 16) {
                std::string end_tid = tid_str.substr(12, 4);
                std::string lp = toLowerCopy(end_tid);
                if (lp == "0800") {
                    res.type = 1;
                    res.label = "Обновление";
                    res.cleanName = "Патч";
                    return res;
                } else if (lp != "0000") {
                    res.type = 2;
                    res.label = "DLC";
                    res.cleanName = "Дополнение " + tid_str;
                    return res;
                }
            }
        }

        // Default: Base Game
        res.type = 0;
        res.label = "Игра";
        res.cleanName = "Базовая игра [v0]";
        return res;
    }
}
