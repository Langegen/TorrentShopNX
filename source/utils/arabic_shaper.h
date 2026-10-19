#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace util
{

class ArabicShaper
{
public:
    // Fast check if string contains any Arabic characters (U+0600..U+06FF, U+0750..U+077F, U+08A0..U+08FF, U+FB50..U+FDFF, U+FE70..U+FEFF)
    static bool hasArabic(const std::string& utf8Str);

    // Shapes Arabic characters (contextual glyph selection into Presentation Forms-B)
    // and performs visual BiDi reordering so that LTR rendering engines (NanoVG / FontStash)
    // display properly connected Right-to-Left Arabic text.
    static std::string shape(const std::string& utf8Str);

private:
    static bool decodeUtf8(const std::string& str, std::vector<uint32_t>& codepoints);
    static std::string encodeUtf8(const std::vector<uint32_t>& codepoints);

    static bool isArabicCodepoint(uint32_t cp);
    static bool isHaraka(uint32_t cp);
    static bool isWhitespace(uint32_t cp);

    static std::vector<uint32_t> shapeRun(const std::vector<uint32_t>& cps);
    static std::string processLine(const std::string& line);
};

} // namespace util
