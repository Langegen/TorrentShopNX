#pragma once

#include <switch.h>

#include <algorithm>
#include <string>
#include <vector>

namespace ui::layout {

inline void useFullScreenWindow() {
    PrintConsole* con = consoleGetDefault();
    if (!con) return;
    consoleSetWindow(con, 0, 0, con->consoleWidth, con->consoleHeight);
}

inline int consoleWidth() {
    PrintConsole* con = consoleGetDefault();
    if (!con) return 80;
    return con->windowWidth > 0 ? con->windowWidth : con->consoleWidth;
}

inline int consoleHeight() {
    PrintConsole* con = consoleGetDefault();
    if (!con) return 45;
    return con->windowHeight > 0 ? con->windowHeight : con->consoleHeight;
}

inline int printableWidth(int padding = 0) {
    return std::max(1, consoleWidth() - 1 - padding);
}

inline size_t utf8CharBytes(unsigned char lead) {
    if ((lead & 0x80) == 0x00) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

inline bool decodeUtf8Codepoint(const std::string& text, size_t& pos, uint32_t& out_codepoint) {
    if (pos >= text.size()) return false;

    const unsigned char lead = static_cast<unsigned char>(text[pos]);
    const size_t len = utf8CharBytes(lead);
    if (len == 1) {
        out_codepoint = lead;
        ++pos;
        return true;
    }
    if ((pos + len) > text.size()) {
        out_codepoint = '?';
        ++pos;
        return false;
    }

    uint32_t codepoint = 0;
    switch (len) {
        case 2:
            codepoint = lead & 0x1F;
            break;
        case 3:
            codepoint = lead & 0x0F;
            break;
        case 4:
            codepoint = lead & 0x07;
            break;
        default:
            out_codepoint = '?';
            ++pos;
            return false;
    }

    for (size_t i = 1; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[pos + i]);
        if ((c & 0xC0) != 0x80) {
            out_codepoint = '?';
            ++pos;
            return false;
        }
        codepoint = (codepoint << 6) | (c & 0x3F);
    }

    out_codepoint = codepoint;
    pos += len;
    return true;
}

inline size_t utf8Length(const std::string& text) {
    size_t count = 0;
    for (size_t i = 0; i < text.size();) {
        i += utf8CharBytes(static_cast<unsigned char>(text[i]));
        ++count;
    }
    return count;
}

inline std::string utf8Prefix(const std::string& text, size_t max_chars) {
    if (max_chars == 0) return {};

    size_t pos = 0;
    size_t count = 0;
    while (pos < text.size() && count < max_chars) {
        pos += utf8CharBytes(static_cast<unsigned char>(text[pos]));
        ++count;
    }
    return text.substr(0, pos);
}

inline std::string trimAsciiSpaces(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && text[begin] == ' ') {
        ++begin;
    }

    size_t end = text.size();
    while (end > begin && text[end - 1] == ' ') {
        --end;
    }
    return text.substr(begin, end - begin);
}

inline std::string ellipsizeUtf8(const std::string& text, size_t max_chars) {
    if (max_chars == 0) return {};
    if (utf8Length(text) <= max_chars) return text;
    if (max_chars <= 3) return utf8Prefix(text, max_chars);
    return utf8Prefix(text, max_chars - 3) + "...";
}

inline std::vector<std::string> wrapTextUtf8(const std::string& text, size_t width, size_t max_lines = 0) {
    std::vector<std::string> lines;
    if (width == 0) {
        lines.push_back({});
        return lines;
    }

    size_t start = 0;
    while (start < text.size()) {
        while (start < text.size() && (text[start] == '\r' || text[start] == '\n')) {
            lines.push_back({});
            if (max_lines > 0 && lines.size() >= max_lines) return lines;
            ++start;
        }
        if (start >= text.size()) break;

        size_t pos = start;
        size_t chars = 0;
        size_t last_space = std::string::npos;

        while (pos < text.size()) {
            const char ch = text[pos];
            if (ch == '\r' || ch == '\n') break;
            const size_t char_len = utf8CharBytes(static_cast<unsigned char>(ch));
            if (chars >= width) break;
            if (ch == ' ') last_space = pos;
            pos += char_len;
            ++chars;
        }

        size_t end = pos;
        if (pos < text.size() && text[pos] != '\r' && text[pos] != '\n' &&
            chars >= width && last_space != std::string::npos && last_space > start) {
            end = last_space;
        }

        if (end <= start) {
            end = std::min(text.size(), start + utf8CharBytes(static_cast<unsigned char>(text[start])));
        }

        std::string line = trimAsciiSpaces(text.substr(start, end - start));
        lines.push_back(line);

        if (max_lines > 0 && lines.size() >= max_lines) {
            if (end < text.size()) {
                lines.back() = ellipsizeUtf8(lines.back(), width);
            }
            return lines;
        }

        start = end;
        while (start < text.size() && text[start] == ' ') ++start;
        while (start < text.size() && (text[start] == '\r' || text[start] == '\n')) ++start;
    }

    if (lines.empty()) {
        lines.push_back({});
    }
    return lines;
}

inline std::vector<std::string> fixedTextBlock(const std::string& text, size_t width, size_t lines) {
    std::vector<std::string> out = wrapTextUtf8(text, width, lines);
    if (out.size() > lines) {
        out.resize(lines);
    }
    while (out.size() < lines) {
        out.emplace_back();
    }
    return out;
}

inline void appendConsoleEncoded(std::string& out, uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
        return;
    }

    if (codepoint == 0x0401) {
        out.push_back(static_cast<char>(0xA8));
        return;
    }
    if (codepoint == 0x0451) {
        out.push_back(static_cast<char>(0xB8));
        return;
    }
    if (codepoint >= 0x0410 && codepoint <= 0x042F) {
        out.push_back(static_cast<char>(0xC0 + (codepoint - 0x0410)));
        return;
    }
    if (codepoint >= 0x0430 && codepoint <= 0x044F) {
        out.push_back(static_cast<char>(0xE0 + (codepoint - 0x0430)));
        return;
    }

    switch (codepoint) {
        case 0x00AB: out.push_back(static_cast<char>(0xAB)); return;
        case 0x00BB: out.push_back(static_cast<char>(0xBB)); return;
        case 0x2116: out.push_back(static_cast<char>(0xB9)); return;
        case 0x2026: out += "..."; return;
        case 0x2013: out.push_back(static_cast<char>(0x96)); return;
        case 0x2014: out.push_back(static_cast<char>(0x97)); return;
        case 0x00A0: out.push_back(' '); return;
        case 0x2018:
        case 0x2019:
        case 0x201A: out.push_back('\''); return;
        case 0x201C:
        case 0x201D:
        case 0x201E: out.push_back('"'); return;
        default:
            out.push_back('?');
            return;
    }
}

inline std::string encodeForConsole(const std::string& text) {
    std::string out;
    out.reserve(text.size() * 2);

    size_t pos = 0;
    while (pos < text.size()) {
        uint32_t codepoint = '?';
        decodeUtf8Codepoint(text, pos, codepoint);
        appendConsoleEncoded(out, codepoint);
    }

    return out;
}

inline std::string separatorLine(char ch = '-') {
    return std::string(printableWidth(), ch);
}

inline int pageCount(int total_items, int page_size) {
    if (page_size <= 0) return 1;
    return std::max(1, (total_items + page_size - 1) / page_size);
}

inline int clampIndex(int value, int total_items) {
    if (total_items <= 0) return 0;
    if (value < 0) return 0;
    if (value >= total_items) return total_items - 1;
    return value;
}

} // namespace ui::layout
