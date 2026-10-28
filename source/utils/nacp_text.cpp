#include "nacp_text.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <zlib.h>

namespace util {

namespace {

size_t boundedLen(const char* s, size_t maxLen) {
    const void* nul = std::memchr(s, 0, maxLen);
    return nul ? static_cast<size_t>(static_cast<const char*>(nul) - s) : maxLen;
}

// Length of a valid UTF-8 sequence starting at s[i], 0 when it is invalid.
size_t utf8SeqLen(const unsigned char* s, size_t i, size_t len) {
    unsigned char c = s[i];
    size_t n;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) n = 2;
    else if (c >= 0xE0 && c <= 0xEF) n = 3;
    else if (c >= 0xF0 && c <= 0xF4) n = 4;
    else return 0;
    if (i + n > len) return 0;
    for (size_t k = 1; k < n; ++k) {
        if ((s[i + k] & 0xC0) != 0x80) return 0;
    }
    unsigned char c1 = s[i + 1];
    if (c == 0xE0 && c1 < 0xA0) return 0;   // overlong
    if (c == 0xED && c1 >= 0xA0) return 0;  // UTF-16 surrogates
    if (c == 0xF0 && c1 < 0x90) return 0;   // overlong
    if (c == 0xF4 && c1 >= 0x90) return 0;  // above U+10FFFF
    return n;
}

// A NUL-terminated field of plain text: valid UTF-8, no control characters.
bool isCleanField(const char* field, size_t size) {
    size_t len = boundedLen(field, size);
    if (len == size) return false;  // no terminator inside the field
    const auto* s = reinterpret_cast<const unsigned char*>(field);
    for (size_t i = 0; i < len;) {
        if (s[i] < 0x20 || s[i] == 0x7F) return false;
        size_t n = utf8SeqLen(s, i, len);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

}  // namespace

std::string sanitizeUtf8(const char* str, size_t maxLen) {
    std::string out;
    if (!str) return out;
    size_t len = boundedLen(str, maxLen);
    const auto* s = reinterpret_cast<const unsigned char*>(str);
    out.reserve(len);
    for (size_t i = 0; i < len;) {
        if (s[i] < 0x20 || s[i] == 0x7F) {
            if (s[i] == '\t' || s[i] == '\n' || s[i] == '\r') out.push_back(' ');
            ++i;
            continue;
        }
        size_t n = utf8SeqLen(s, i, len);
        if (n == 0) {
            ++i;
            continue;
        }
        out.append(str + i, n);
        i += n;
    }
    return out;
}

std::string nacpText(const char* field, size_t size) {
    if (!field || !isCleanField(field, size)) return "";
    return std::string(field, boundedLen(field, size));
}

bool unpackNacpTitles(void* table, size_t tableSize) {
    if (!table || tableSize < kNacpLangEntrySize + 2) return false;
    const auto* src = static_cast<const unsigned char*>(table);
    size_t compSize = static_cast<size_t>(src[0]) | (static_cast<size_t>(src[1]) << 8);
    if (compSize == 0 || compSize > tableSize - 2) return false;

    // Room for more entries than the plain table holds (newer firmware has
    // more languages); only the first tableSize bytes are kept.
    std::vector<unsigned char> out(tableSize * 4);
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(src + 2);
    zs.avail_in = static_cast<uInt>(compSize);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    int rc = inflate(&zs, Z_FINISH);
    size_t consumed = zs.total_in;
    size_t produced = zs.total_out;
    inflateEnd(&zs);

    // A plain table almost never forms a deflate stream that ends exactly at
    // the stated length and inflates to whole entries of clean text.
    if (rc != Z_STREAM_END || consumed != compSize) return false;
    if (produced == 0 || produced % kNacpLangEntrySize != 0) return false;
    bool anyName = false;
    for (size_t off = 0; off < produced; off += kNacpLangEntrySize) {
        const char* entry = reinterpret_cast<const char*>(out.data() + off);
        if (!isCleanField(entry, kNacpLangNameSize) ||
            !isCleanField(entry + kNacpLangNameSize, kNacpLangEntrySize - kNacpLangNameSize)) {
            return false;
        }
        if (entry[0] != '\0') anyName = true;
    }
    if (!anyName) return false;

    std::memset(table, 0, tableSize);
    size_t keep = produced < tableSize ? produced : tableSize;
    keep -= keep % kNacpLangEntrySize;
    std::memcpy(table, out.data(), keep);
    return true;
}

}  // namespace util
