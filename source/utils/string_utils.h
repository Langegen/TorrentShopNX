#pragma once
#include <string>
#include <string_view>
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
    inline std::string toLowerUtf8(std::string_view s) {
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
        int type = 0;              // 0: Base (MAIN), 1: Update (UPD), 2: DLC (DLC), 3: Mod/Extra (OTH)
        std::string label;         // "MAIN", "UPD", "DLC", "OTH"
        std::string cleanName;     // Clean, human-readable name without repeating the game title or Atmosphere paths
        std::string version;       // Update version as in the file name ("[v65536]", "v1.2.0"), empty if unknown
    };

    inline std::string extractPatchVersionString(const std::string& filename) {
        // Strictly look for integer version: [v196608], (v65536), etc.
        for (size_t i = 0; i < filename.size(); ++i) {
            if (filename[i] == '[' || filename[i] == '(') {
                char closeChar = (filename[i] == '[') ? ']' : ')';
                size_t closePos = filename.find(closeChar, i + 1);
                if (closePos != std::string::npos && closePos - i > 2) {
                    std::string token = filename.substr(i + 1, closePos - i - 1);
                    if ((token[0] == 'v' || token[0] == 'V') && token.size() > 1) {
                        bool allDigits = true;
                        for (size_t k = 1; k < token.size(); ++k) {
                            if (!std::isdigit(static_cast<unsigned char>(token[k]))) {
                                allDigits = false;
                                break;
                            }
                        }
                        if (allDigits) {
                            try {
                                unsigned long vval = std::stoul(token.substr(1));
                                if (vval > 0) {
                                    return "[v" + std::to_string(vval) + "]";
                                }
                            } catch (...) {}
                        }
                    }
                }
            }
        }

        // Fallback: check other delimiters like _v196608 or v196608
        for (size_t i = 0; i < filename.size(); ++i) {
            char delim = filename[i];
            if (delim == '_' || delim == ' ') {
                if (i + 2 < filename.size() && (filename[i + 1] == 'v' || filename[i + 1] == 'V') &&
                    std::isdigit(static_cast<unsigned char>(filename[i + 2]))) {
                    size_t start = i + 2;
                    size_t end = start;
                    while (end < filename.size() && std::isdigit(static_cast<unsigned char>(filename[end]))) {
                        end++;
                    }
                    if (end > start) {
                        try {
                            unsigned long vval = std::stoul(filename.substr(start, end - start));
                            if (vval > 0) {
                                return "[v" + std::to_string(vval) + "]";
                            }
                        } catch (...) {}
                    }
                }
            }
        }

        return "";
    }

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
            res.label = "OTH";

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

        // --- Package (.nsp, .nsz, .xci, .xcz) classification ---

        // 1. Check for 16-character hexadecimal Title ID in filename
        uint64_t tid = 0;
        for (size_t i = 0; i < filename.size(); ++i) {
            if (filename[i] == '[' || filename[i] == '(') {
                size_t closePos = filename.find(filename[i] == '[' ? ']' : ')', i + 1);
                if (closePos != std::string::npos && (closePos - i - 1 == 16)) {
                    std::string hexStr = filename.substr(i + 1, 16);
                    bool isHex = true;
                    for (char c : hexStr) {
                        if (!std::isxdigit(static_cast<unsigned char>(c))) { isHex = false; break; }
                    }
                    if (isHex) {
                        try {
                            tid = std::stoull(hexStr, nullptr, 16);
                            break;
                        } catch (...) {}
                    }
                }
            }
        }

        // 2. Check explicit base game markers (v0, Base, v1.0.0)
        bool is_explicit_base = (
            lower.find("[v0]") != std::string::npos ||
            lower.find("(v0)") != std::string::npos ||
            lower.find("[v0.") != std::string::npos ||
            lower.find("(v0.") != std::string::npos ||
            lower.find("_v0.") != std::string::npos ||
            lower.find("_v0_") != std::string::npos ||
            lower.find(" v0 ") != std::string::npos ||
            lower.find(" v0.") != std::string::npos ||
            lower.find(" v0]") != std::string::npos ||
            lower.find("[base]") != std::string::npos ||
            lower.find("(base)") != std::string::npos ||
            lower.find("_base_") != std::string::npos ||
            lower.find("[base game]") != std::string::npos ||
            lower.find("(base game)") != std::string::npos ||
            lower.find("[v1.0.0]") != std::string::npos ||
            lower.find("(v1.0.0)") != std::string::npos ||
            lower.find("[v1.0]") != std::string::npos
        );

        // 3. Nintendo Switch Title ID type mask:
        //    (tid & 0xFFF) == 0      -> Application (Base game)
        //    (tid & 0xFFF) == 0x800  -> Patch (Update)
        //    Otherwise               -> AddOnContent (DLC)
        bool is_base_tid = (tid != 0 && (tid & 0xFFFULL) == 0);
        bool is_patch_tid = (tid != 0 && (tid & 0xFFFULL) == 0x800ULL);
        bool is_dlc_tid = (tid != 0 && !is_base_tid && !is_patch_tid);

        // Explicit DLC checks by name
        bool is_dlc_name = (lower.find("[dlc") != std::string::npos ||
                            lower.find("(dlc") != std::string::npos ||
                            lower.find(" dlc ") != std::string::npos ||
                            lower.find("[addon") != std::string::npos);

        // Priority 1: DLC (by Title ID or explicit DLC in name)
        // Switch DLC Title IDs have (tid & 0xFFF) != 0 and != 0x800. Even if DLC is v0, it is DLC.
        if (is_dlc_name || is_dlc_tid) {
            res.type = 2;
            res.label = "DLC";
            size_t dlc_pos = filename.find("[DLC");
            if (dlc_pos == std::string::npos) dlc_pos = filename.find("(DLC");
            if (dlc_pos == std::string::npos) dlc_pos = filename.find("[dlc");
            if (dlc_pos == std::string::npos) dlc_pos = filename.find("(dlc");

            if (dlc_pos != std::string::npos) {
                size_t close_bracket = filename.find_first_of("])", dlc_pos);
                if (close_bracket != std::string::npos) {
                    std::string dlc_str = filename.substr(dlc_pos + 1, close_bracket - dlc_pos - 1);
                    if (toLowerUtf8(dlc_str).find("dlc") == 0) {
                        dlc_str = dlc_str.substr(3);
                        while (!dlc_str.empty() && (dlc_str.front() == ' ' || dlc_str.front() == ':' || dlc_str.front() == '-')) {
                            dlc_str.erase(dlc_str.begin());
                        }
                    }
                    res.cleanName = dlc_str.empty() ? filename : dlc_str;
                } else {
                    res.cleanName = filename;
                }
            } else if (tid != 0) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "DLC %016llX", (unsigned long long)tid);
                res.cleanName = buf;
            } else {
                res.cleanName = filename;
            }
            return res;
        }

        // Priority 2: Patch / Update by Title ID (ends in 0x800)
        if (is_patch_tid) {
            res.type = 1;
            res.label = "UPD";
            std::string verStr = extractPatchVersionString(filename);
            res.version = verStr;
            res.cleanName = verStr.empty() ? "Патч" : ("Патч " + verStr);
            return res;
        }

        // Priority 3: Base Game
        // Either explicit base marker (e.g. [v0]) or Base Title ID (ends in 0x000)
        if (is_base_tid || is_explicit_base) {
            res.type = 0;
            res.label = "MAIN";
            res.cleanName = "Базовая игра [v0]";
            return res;
        }

        // Check for Update / Patch by version string or explicit keywords
        std::string verStr = extractPatchVersionString(filename);
        if (!verStr.empty()) {
            res.type = 1;
            res.label = "UPD";
            res.version = verStr;
            res.cleanName = "Патч " + verStr;
            return res;
        }

        if (lower.find("[upd") != std::string::npos || lower.find("(upd") != std::string::npos ||
            lower.find("[patch") != std::string::npos || lower.find("(patch") != std::string::npos ||
            lower.find("_update") != std::string::npos || lower.find(" update ") != std::string::npos) {
            res.type = 1;
            res.label = "UPD";
            res.cleanName = "Патч";
            return res;
        }

        // Default: Base Game
        res.type = 0;
        res.label = "MAIN";
        res.cleanName = "Базовая игра [v0]";
        return res;
    }
}
