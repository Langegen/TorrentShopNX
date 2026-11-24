#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace util {

/* Produces a log-safe one-line summary of an HTTP response body:
 * - binary payloads (JPEG/PNG/GIF/WebP magic, NUL byte, or >10% ASCII control bytes
 *   in the first 512 bytes) become "<binary NB>" so raw images never flood logs;
 * - text payloads have ASCII whitespace collapsed and are capped at 160 chars. */
inline std::string summarizeHttpErrorBody(const std::string& body) {
    if (body.empty()) return {};

    const size_t n = body.size();
    const auto u8 = [&](size_t idx) -> uint8_t {
        return static_cast<uint8_t>(body[idx]);
    };

    bool is_binary = false;
    if (n >= 3 && u8(0) == 0xFF && u8(1) == 0xD8 && u8(2) == 0xFF) is_binary = true; // JPEG
    else if (n >= 4 && u8(0) == 0x89 && u8(1) == 0x50 && u8(2) == 0x4E && u8(3) == 0x47) is_binary = true; // PNG
    else if (n >= 4 && u8(0) == 'G' && u8(1) == 'I' && u8(2) == 'F' && u8(3) == '8') is_binary = true; // GIF
    else if (n >= 12 && u8(0) == 'R' && u8(1) == 'I' && u8(2) == 'F' && u8(3) == 'F' &&
             u8(8) == 'W' && u8(9) == 'E' && u8(10) == 'B' && u8(11) == 'P') is_binary = true; // WebP

    if (!is_binary) {
        const size_t probe = (n < 512) ? n : 512;
        size_t ctrl_count = 0;
        for (size_t i = 0; i < probe; ++i) {
            const uint8_t c = u8(i);
            if (c == 0) {
                is_binary = true;
                break;
            }
            if (c < 0x20 && c != '\t' && c != '\r' && c != '\n') {
                ++ctrl_count;
            }
        }
        if (!is_binary && probe > 0 && ctrl_count * 10 > probe) {
            is_binary = true;
        }
    }

    if (is_binary) {
        return "<binary " + std::to_string(n) + "B>";
    }

    std::string out;
    out.reserve((n < 164) ? n : 164);
    bool in_ws = true; // trim leading whitespace
    bool truncated = false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(body[i]);
        const bool ws = (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f');
        if (ws) {
            if (!in_ws) {
                if (out.size() >= 160) {
                    truncated = true;
                    break;
                }
                out.push_back(' ');
                in_ws = true;
            }
            continue;
        }
        in_ws = false;
        if (out.size() >= 160) {
            truncated = true;
            break;
        }
        out.push_back(static_cast<char>(c));
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    if (truncated) {
        if (out.size() > 157) out.resize(157);
        while (!out.empty() && out.back() == ' ') out.pop_back();
        out += "...";
    }
    return out;
}

} // namespace util
