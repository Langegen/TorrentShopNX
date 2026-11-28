#include "utils/keyboard_layouts.h"

#include <cassert>
#include <iostream>
#include <string>

static void expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

int main() {
    using namespace util::kbd;

    // 1. All 3 layouts x 2 shift states x 4x12 keys are non-empty & valid
    for (Layout l : {Layout::EN, Layout::RU, Layout::SYM}) {
        for (bool s : {false, true}) {
            for (int r = 0; r < kRows; ++r) {
                for (int c = 0; c < kCols; ++c) {
                    const char* tok = keyText(l, s, r, c);
                    expect(tok != nullptr && tok[0] != '\0', "every grid cell must be non-empty");
                }
            }
        }
    }
    expect(std::string(keyText(Layout::EN, false, -1, 0)).empty(), "out of bounds row returns empty");
    expect(std::string(keyText(Layout::EN, false, 0, 12)).empty(), "out of bounds col returns empty");

    // 2. UTF-8 codepoint counting & popping without breaking multibyte sequences
    expect(utf8CodepointCount("") == 0, "empty codepoint count");
    expect(utf8CodepointCount("Mario 64") == 8, "ascii codepoint count");
    expect(utf8CodepointCount("Привет!") == 7, "cyrillic + ascii codepoint count");

    std::string word = "Игр";
    utf8PopBack(word);
    expect(word == "Иг", "utf8PopBack removes whole 2-byte char");
    utf8PopBack(word);
    expect(word == "И", "utf8PopBack leaves 1 cyrillic char");
    utf8PopBack(word);
    expect(word.empty(), "utf8PopBack empties string");
    utf8PopBack(word);
    expect(word.empty(), "utf8PopBack on empty string is safe");

    // 3. Multibyte clamping
    expect(clampUtf8Codepoints("АБВГД", 3) == "АБВ", "clamp stops on UTF-8 boundary");
    expect(clampUtf8Codepoints("abc", 0) == "abc", "clamp max<=0 falls back cleanly");

    // 4. Disable bitmask & max length enforcement
    expect(isTokenDisabled(" ", kDisableSpace), "space disabled when flag set");
    expect(!isTokenDisabled("a", kDisableSpace), "letters allowed when space disabled");
    expect(isTokenDisabled("@", kDisableAt), "@ disabled");
    expect(isTokenDisabled("https://", kDisableForwSlash), "http prefix rejected when / disabled");
    expect(isTokenDisabled("7", kDisableNumbers), "digit rejected when numbers disabled");
    expect(isTokenDisabled("%", kDisableUsername), "% rejected for username preset");

    std::string buf = "ab";
    expect(appendToken(buf, "cd", 4, kDisableNone), "append fits within max");
    expect(buf == "abcd", "buf equals abcd");
    expect(!appendToken(buf, "e", 4, kDisableNone), "append blocked at max length");
    expect(buf == "abcd", "buf unchanged when rejected");

    std::string ru = "Ми";
    expect(appendToken(ru, "р", 3, kDisableNone), "cyrillic append fits by codepoints");
    expect(!appendToken(ru, "!", 3, kDisableNone), "cyrillic max codepoint limit enforced");

    // 5. Tail display slicing preserves UTF-8 and prepends ellipsis
    expect(tailDisplaySlice("short", 10) == "short", "short string untouched");
    std::string sliced = tailDisplaySlice("АБВГДЕЖЗ", 5);
    expect(utf8CodepointCount(sliced) == 5, "sliced UTF-8 length matches budget");
    expect(sliced.find("\xE2\x80\xA6") == 0, "sliced starts with UTF-8 ellipsis");
    expect(sliced == "\xE2\x80\xA6" "ДЕЖЗ", "sliced keeps tail cyrillic chars intact");

    // 6. Initial layout heuristic
    expect(prefersLatinLayout("http://192.168.1.100:8090"), "URL prefers EN");
    expect(prefersLatinLayout("sdmc:/roms/"), "path prefers EN");
    expect(prefersLatinLayout("@langegen"), "contact prefers EN");
    expect(!prefersLatinLayout(""), "empty does not force EN");
    expect(!prefersLatinLayout("Марио"), "cyrillic does not force EN");

    std::cout << "ALL test_keyboard_layouts assertions passed.\n";
    return 0;
}
