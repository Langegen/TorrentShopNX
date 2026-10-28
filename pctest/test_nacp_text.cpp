// Checks util::unpackNacpTitles / util::sanitizeUtf8 (SDK 20+ compressed NACP titles).
// Build: g++ -std=c++17 -Isource pctest/test_nacp_text.cpp source/utils/nacp_text.cpp -lz
#include "utils/nacp_text.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using util::kNacpLangEntrySize;
using util::kNacpLangNameSize;
using util::kNacpTitleTableSize;

static void setEntry(std::vector<unsigned char>& t, int i, const char* name, const char* pub) {
    std::memset(&t[i * kNacpLangEntrySize], 0, kNacpLangEntrySize);
    std::memcpy(&t[i * kNacpLangEntrySize], name, std::strlen(name));
    std::memcpy(&t[i * kNacpLangEntrySize + kNacpLangNameSize], pub, std::strlen(pub));
}

// The way SDK 20+ stores it: LE16 length + raw deflate of the entries.
static std::vector<unsigned char> compressTable(const std::vector<unsigned char>& plain) {
    std::vector<unsigned char> out(kNacpTitleTableSize, 0);
    z_stream zs{};
    deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    zs.next_in = const_cast<Bytef*>(plain.data());
    zs.avail_in = static_cast<uInt>(plain.size());
    zs.next_out = out.data() + 2;
    zs.avail_out = static_cast<uInt>(out.size() - 2);
    int rc = deflate(&zs, Z_FINISH);
    size_t n = zs.total_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) { std::printf("deflate failed\n"); ++g_fail; }
    out[0] = static_cast<unsigned char>(n & 0xFF);
    out[1] = static_cast<unsigned char>(n >> 8);
    return out;
}

static std::string name(const std::vector<unsigned char>& t, int i) {
    return util::sanitizeUtf8(reinterpret_cast<const char*>(&t[i * kNacpLangEntrySize]), kNacpLangNameSize);
}

int main() {
    // 1. Compressed 16-entry table is unpacked
    {
        std::vector<unsigned char> plain(kNacpTitleTableSize, 0);
        for (int i = 0; i < 16; ++i) setEntry(plain, i, "The Legend of Zelda: Breath of the Wild", "Nintendo");
        setEntry(plain, 10, "Легенда о Зельде", "Nintendo");
        auto t = compressTable(plain);
        CHECK(t[0] | (t[1] << 8));
        CHECK(util::unpackNacpTitles(t.data(), t.size()));
        CHECK(t == plain);
        CHECK(name(t, 1) == "The Legend of Zelda: Breath of the Wild");
        CHECK(name(t, 10) == "Легенда о Зельде");
    }
    // 2. Compressed table with 32 entries (newer firmware): first 16 kept
    {
        std::vector<unsigned char> plain(kNacpTitleTableSize * 2, 0);
        for (int i = 0; i < 32; ++i) setEntry(plain, i, i < 16 ? "Game" : "Extra", "Pub");
        auto t = compressTable(plain);
        CHECK(util::unpackNacpTitles(t.data(), t.size()));
        CHECK(name(t, 15) == "Game");
        CHECK(std::memcmp(t.data(), plain.data(), kNacpTitleTableSize) == 0);
    }
    // 3. Single language game (short stream, length < 256)
    {
        std::vector<unsigned char> plain(kNacpTitleTableSize, 0);
        setEntry(plain, 0, "Some Game", "Dev");
        auto t = compressTable(plain);
        CHECK(t[1] == 0);
        CHECK(util::unpackNacpTitles(t.data(), t.size()));
        CHECK(name(t, 0) == "Some Game");
    }
    // 4. Plain tables are left untouched
    {
        const char* names[] = {"Super Mario Odyssey", "E", "A ", "Ab", "\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0"};
        for (const char* n : names) {
            std::vector<unsigned char> t(kNacpTitleTableSize, 0);
            for (int i = 0; i < 16; ++i) setEntry(t, i, n, "Nintendo");
            auto before = t;
            CHECK(!util::unpackNacpTitles(t.data(), t.size()));
            CHECK(t == before);
        }
        std::vector<unsigned char> empty(kNacpTitleTableSize, 0);
        CHECK(!util::unpackNacpTitles(empty.data(), empty.size()));
    }
    // 5. Garbage is left untouched (bytes from the user's crash log)
    {
        std::vector<unsigned char> t(kNacpTitleTableSize, 0);
        const unsigned char junk[] = {0x45, 0x01, 0xED, 0xD6, 0x31, 0x0A, 0xC2, 0x40, 0x10};
        std::memcpy(t.data(), junk, sizeof(junk));
        auto before = t;
        CHECK(!util::unpackNacpTitles(t.data(), t.size()));
        CHECK(t == before);
    }
    // 6. sanitizeUtf8
    {
        const char bad[] = "\x45\x01\xED\xD6\x31";
        CHECK(util::sanitizeUtf8(bad, sizeof(bad)) == "E1");
        CHECK(util::sanitizeUtf8("Mario\tKart\n8", 64) == "Mario Kart 8");
        CHECK(util::sanitizeUtf8(std::string("Pok\xC3\xA9mon")) == "Pok\xC3\xA9mon");
        CHECK(util::sanitizeUtf8(std::string("\xC3")) == "");
        CHECK(util::sanitizeUtf8(std::string("\xF0\x9F\x8E\xAE ok")) == "\xF0\x9F\x8E\xAE ok");
        CHECK(util::sanitizeUtf8(std::string("\xED\xA0\x80x")) == "x");  // surrogate
        const char noNul[4] = {'a', 'b', 'c', 'd'};
        CHECK(util::sanitizeUtf8(noNul, 3) == "abc");
        CHECK(util::sanitizeUtf8(nullptr, 5) == "");
    }
    // 7. nacpText rejects binary data
    {
        char field[0x200] = {};
        std::memcpy(field, "\x45\x01\xED\xD6", 4);
        CHECK(util::nacpText(field, sizeof(field)) == "");
        std::memset(field, 0, sizeof(field));
        std::strcpy(field, "Pok\xC3\xA9mon Legends: Z-A");
        CHECK(util::nacpText(field, sizeof(field)) == "Pok\xC3\xA9mon Legends: Z-A");
        std::memset(field, 'a', sizeof(field));   // no terminator
        CHECK(util::nacpText(field, sizeof(field)) == "");
    }

    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
