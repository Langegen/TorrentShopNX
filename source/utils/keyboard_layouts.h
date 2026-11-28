#pragma once

#include <cstddef>
#include <string>

namespace util::kbd {

constexpr int kRows = 4;
constexpr int kCols = 12;

enum class Layout : int {
    EN = 0,
    RU = 1,
    SYM = 2,
};

enum class ShiftMode : int {
    OFF = 0,
    ONCE = 1,
    LOCK = 2,
};

// Mirror brls::KeyboardKeyDisableBitmask values without requiring Borealis headers.
constexpr int kDisableNone         = 0;
constexpr int kDisableSpace        = 1 << 0;
constexpr int kDisableAt           = 1 << 1;
constexpr int kDisablePercent      = 1 << 2;
constexpr int kDisableForwSlash    = 1 << 3;
constexpr int kDisableBackSlash    = 1 << 4;
constexpr int kDisableNumbers      = 1 << 5;
constexpr int kDisableDownloadCode = 1 << 6;
constexpr int kDisableUsername     = 1 << 7;

inline const char* keyText(Layout layout, bool shifted, int row, int col) {
    if (row < 0 || row >= kRows || col < 0 || col >= kCols) return "";

    static constexpr const char* kEnLower[kRows][kCols] = {
        { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "_" },
        { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]" },
        { "a", "s", "d", "f", "g", "h", "j", "k", "l", ":", ";", "'" },
        { "z", "x", "c", "v", "b", "n", "m", ".", ",", "/", "?", "@" },
    };

    static constexpr const char* kEnUpper[kRows][kCols] = {
        { "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "+", "=" },
        { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "{", "}" },
        { "A", "S", "D", "F", "G", "H", "J", "K", "L", "\"", "~", "`" },
        { "Z", "X", "C", "V", "B", "N", "M", "<", ">", "\\", "|", "#" },
    };

    static constexpr const char* kRuLower[kRows][kCols] = {
        { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "_" },
        { "й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з", "х", "ъ" },
        { "ф", "ы", "в", "а", "п", "р", "о", "л", "д", "ж", "э", "ё" },
        { "я", "ч", "с", "м", "и", "т", "ь", "б", "ю", ".", ",", "?" },
    };

    static constexpr const char* kRuUpper[kRows][kCols] = {
        { "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "+", "=" },
        { "Й", "Ц", "У", "К", "Е", "Н", "Г", "Ш", "Щ", "З", "Х", "Ъ" },
        { "Ф", "Ы", "В", "А", "П", "Р", "О", "Л", "Д", "Ж", "Э", "Ё" },
        { "Я", "Ч", "С", "М", "И", "Т", "Ь", "Б", "Ю", "!", ":", "/" },
    };

    static constexpr const char* kSymLower[kRows][kCols] = {
        { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", ".", ":" },
        { "/", "\\", "-", "_", "=", "+", "*", "&", "%", "$", "#", "@" },
        { "!", "?", "~", "`", "'", "\"", ";", ",", "(", ")", "[", "]" },
        { "{", "}", "<", ">", "|", "^", "http://", "https://", "sdmc:/", "192.168.", ".zip", ".json" },
    };

    static constexpr const char* kSymUpper[kRows][kCols] = {
        { "á", "à", "â", "ä", "ã", "å", "é", "è", "ê", "ë", "í", "ì" },
        { "î", "ï", "ó", "ò", "ô", "ö", "õ", "ú", "ù", "û", "ü", "ñ" },
        { "ç", "ß", "ý", "ő", "ű", "Á", "É", "Í", "Ó", "Ú", "Ö", "Ü" },
        { "Ñ", "Ç", "Ä", "Ő", "Ű", "€", "£", "¥", "№", "—", "«", "»" },
    };

    switch (layout) {
        case Layout::RU:
            return shifted ? kRuUpper[row][col] : kRuLower[row][col];
        case Layout::SYM:
            return shifted ? kSymUpper[row][col] : kSymLower[row][col];
        case Layout::EN:
        default:
            return shifted ? kEnUpper[row][col] : kEnLower[row][col];
    }
}

inline size_t utf8CodepointCount(const std::string& s) {
    size_t count = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

inline void utf8PopBack(std::string& s) {
    if (s.empty()) return;
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
        s.pop_back();
    }
    if (!s.empty()) {
        s.pop_back();
    }
}

inline std::string clampUtf8Codepoints(const std::string& s, int maxCodepoints) {
    const size_t limit = static_cast<size_t>(maxCodepoints > 0 ? maxCodepoints : 256);
    size_t cp = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t charLen = 1;
        if ((c & 0x80) == 0) charLen = 1;
        else if ((c & 0xE0) == 0xC0) charLen = 2;
        else if ((c & 0xF0) == 0xE0) charLen = 3;
        else if ((c & 0xF8) == 0xF0) charLen = 4;
        if (i + charLen > s.size()) break;
        if (cp >= limit) break;
        i += charLen;
        ++cp;
    }
    return s.substr(0, i);
}

inline bool isTokenDisabled(const std::string& token, int bitmask) {
    if (bitmask == kDisableNone || token.empty()) return false;
    for (unsigned char ch : token) {
        if ((bitmask & kDisableSpace) && ch == ' ') return true;
        if ((bitmask & kDisableAt) && ch == '@') return true;
        if ((bitmask & kDisablePercent) && ch == '%') return true;
        if ((bitmask & kDisableForwSlash) && ch == '/') return true;
        if ((bitmask & kDisableBackSlash) && ch == '\\') return true;
        if ((bitmask & kDisableNumbers) && ch >= '0' && ch <= '9') return true;
        if ((bitmask & kDisableUsername) && (ch == '@' || ch == '%' || ch == '\\')) return true;
    }
    return false;
}

inline bool appendToken(std::string& text, const std::string& token, int maxCodepoints, int bitmask = kDisableNone) {
    if (token.empty() || isTokenDisabled(token, bitmask)) return false;
    const size_t limit = static_cast<size_t>(maxCodepoints > 0 ? maxCodepoints : 256);
    const size_t curLen = utf8CodepointCount(text);
    const size_t addLen = utf8CodepointCount(token);
    if (curLen + addLen > limit) return false;
    text += token;
    return true;
}

inline std::string tailDisplaySlice(const std::string& text, size_t maxVisibleCodepoints) {
    if (maxVisibleCodepoints <= 1) return text;
    const size_t total = utf8CodepointCount(text);
    if (total <= maxVisibleCodepoints) return text;

    const size_t skip = total - maxVisibleCodepoints + 1;
    size_t cp = 0;
    size_t i = 0;
    while (i < text.size() && cp < skip) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t charLen = 1;
        if ((c & 0x80) == 0) charLen = 1;
        else if ((c & 0xE0) == 0xC0) charLen = 2;
        else if ((c & 0xF0) == 0xE0) charLen = 3;
        else if ((c & 0xF8) == 0xF0) charLen = 4;
        i += (i + charLen <= text.size()) ? charLen : (text.size() - i);
        ++cp;
    }
    return std::string("\xE2\x80\xA6") + text.substr(i);
}

inline bool prefersLatinLayout(const std::string& initialText) {
    return initialText.find("://") != std::string::npos ||
           initialText.find("sdmc:") != std::string::npos ||
           initialText.find('/') != std::string::npos ||
           initialText.find('@') != std::string::npos;
}

} // namespace util::kbd
