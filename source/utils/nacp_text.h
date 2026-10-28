#pragma once

// Game names and versions read from a NACP (control.nacp).
//
// Games built with SDK 20+ may store the title table (16 x 0x300 bytes at
// offset 0) compressed: a 2-byte little-endian length, then a raw DEFLATE
// stream that inflates to the usual 0x300-byte language entries. libnx does
// not unpack it, so the name read from such a NACP is binary garbage, and
// writing it into JSON used to crash the game manager.

#include <cstddef>
#include <string>

namespace util {

// Size of the NACP title table and of one language entry (name 0x200 + publisher 0x100).
constexpr size_t kNacpTitleTableSize = 0x3000;
constexpr size_t kNacpLangEntrySize = 0x300;
constexpr size_t kNacpLangNameSize = 0x200;

// Unpacks a compressed title table in place. Returns true when it was
// compressed and is now a plain table; leaves a plain table untouched.
bool unpackNacpTitles(void* table, size_t tableSize = kNacpTitleTableSize);

// Text up to the first NUL (at most maxLen bytes) with invalid UTF-8 and
// control characters dropped: always safe to show and to put into JSON.
std::string sanitizeUtf8(const char* s, size_t maxLen);
inline std::string sanitizeUtf8(const std::string& s) { return sanitizeUtf8(s.data(), s.size()); }

// A NACP text field (game name, publisher) when it is plain text, "" when it
// holds binary data, e.g. a compressed table that could not be unpacked.
std::string nacpText(const char* field, size_t size);

}  // namespace util
