#pragma once
#include <string>
#include <algorithm>
#include <cctype>

namespace util {
    inline std::string toLowerCopy(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
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
}
