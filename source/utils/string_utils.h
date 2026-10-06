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
}
