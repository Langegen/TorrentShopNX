#include "arabic_shaper.h"
#include <algorithm>
#include <unordered_map>
#include <sstream>

namespace util
{

struct ArabicForms
{
    uint32_t isolated;
    uint32_t final_form;
    uint32_t initial;
    uint32_t medial;
    bool connects_left;
};

// Unicode Arabic character mapping to Presentation Forms-B and Forms-A
static const std::unordered_map<uint32_t, ArabicForms> s_arabicMap = {
    // Basic Arabic (U+0621 - U+064A)
    { 0x0621, { 0xFE80, 0xFE80, 0xFE80, 0xFE80, false } }, // Hamza (ء)
    { 0x0622, { 0xFE81, 0xFE82, 0xFE81, 0xFE82, false } }, // Alef with Madda Above (آ)
    { 0x0623, { 0xFE83, 0xFE84, 0xFE83, 0xFE84, false } }, // Alef with Hamza Above (أ)
    { 0x0624, { 0xFE85, 0xFE86, 0xFE85, 0xFE86, false } }, // Waw with Hamza Above (ؤ)
    { 0x0625, { 0xFE87, 0xFE88, 0xFE87, 0xFE88, false } }, // Alef with Hamza Below (إ)
    { 0x0626, { 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C, true  } }, // Yeh with Hamza Above (ئ)
    { 0x0627, { 0xFE8D, 0xFE8E, 0xFE8D, 0xFE8E, false } }, // Alef (ا)
    { 0x0628, { 0xFE8F, 0xFE90, 0xFE91, 0xFE92, true  } }, // Beh (ب)
    { 0x0629, { 0xFE93, 0xFE94, 0xFE93, 0xFE94, false } }, // Teh Marbuta (ة)
    { 0x062A, { 0xFE95, 0xFE96, 0xFE97, 0xFE98, true  } }, // Teh (ت)
    { 0x062B, { 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C, true  } }, // Theh (ث)
    { 0x062C, { 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0, true  } }, // Jeem (ج)
    { 0x062D, { 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4, true  } }, // Hah (ح)
    { 0x062E, { 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8, true  } }, // Khah (خ)
    { 0x062F, { 0xFEA9, 0xFEAA, 0xFEA9, 0xFEAA, false } }, // Dal (د)
    { 0x0630, { 0xFEAB, 0xFEAC, 0xFEAB, 0xFEAC, false } }, // Thal (ذ)
    { 0x0631, { 0xFEAD, 0xFEAE, 0xFEAD, 0xFEAE, false } }, // Reh (ر)
    { 0x0632, { 0xFEAF, 0xFEB0, 0xFEAF, 0xFEB0, false } }, // Zain (ز)
    { 0x0633, { 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4, true  } }, // Seen (س)
    { 0x0634, { 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8, true  } }, // Sheen (ش)
    { 0x0635, { 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC, true  } }, // Sad (ص)
    { 0x0636, { 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0, true  } }, // Dad (ض)
    { 0x0637, { 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4, true  } }, // Tah (ط)
    { 0x0638, { 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8, true  } }, // Zah (ظ)
    { 0x0639, { 0xFEC9, 0xFECA, 0xFECB, 0xFECC, true  } }, // Ain (ع)
    { 0x063A, { 0xFECD, 0xFECE, 0xFECF, 0xFED0, true  } }, // Ghain (غ)
    { 0x0640, { 0x0640, 0x0640, 0x0640, 0x0640, true  } }, // Tatweel (ـ)
    { 0x0641, { 0xFED1, 0xFED2, 0xFED3, 0xFED4, true  } }, // Feh (ف)
    { 0x0642, { 0xFED5, 0xFED6, 0xFED7, 0xFED8, true  } }, // Qaf (ق)
    { 0x0643, { 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC, true  } }, // Kaf (ك)
    { 0x0644, { 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0, true  } }, // Lam (ل)
    { 0x0645, { 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4, true  } }, // Meem (م)
    { 0x0646, { 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8, true  } }, // Noon (ن)
    { 0x0647, { 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC, true  } }, // Heh (ه)
    { 0x0648, { 0xFEED, 0xFEEE, 0xFEED, 0xFEEE, false } }, // Waw (و)
    { 0x0649, { 0xFEEF, 0xFEF0, 0xFEEF, 0xFEF0, false } }, // Alef Maksura (ى)
    { 0x064A, { 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4, true  } }, // Yeh (ي)

    // Persian / Urdu additions
    { 0x067E, { 0xFB56, 0xFB57, 0xFB58, 0xFB59, true  } }, // Peh (پ)
    { 0x0686, { 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D, true  } }, // Tcheh (چ)
    { 0x0698, { 0xFB8A, 0xFB8B, 0xFB8A, 0xFB8B, false } }, // Zheh (ژ)
    { 0x06AF, { 0xFB92, 0xFB93, 0xFB94, 0xFB95, true  } }  // Gaf (گ)
};

// Lam-Alef ligatures: key is the second character (Alef variant)
struct LamAlefPair
{
    uint32_t isolated;
    uint32_t final_form;
};

static const std::unordered_map<uint32_t, LamAlefPair> s_lamAlefMap = {
    { 0x0622, { 0xFEF5, 0xFEF6 } }, // ل + آ
    { 0x0623, { 0xFEF7, 0xFEF8 } }, // ل + أ
    { 0x0625, { 0xFEF9, 0xFEFA } }, // ل + إ
    { 0x0627, { 0xFEFB, 0xFEFC } }  // ل + ا
};

bool ArabicShaper::decodeUtf8(const std::string& str, std::vector<uint32_t>& codepoints)
{
    codepoints.clear();
    codepoints.reserve(str.size());

    size_t i = 0;
    size_t len = str.size();

    while (i < len)
    {
        uint8_t c = static_cast<uint8_t>(str[i]);
        if (c < 0x80)
        {
            codepoints.push_back(c);
            i += 1;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            if (i + 1 >= len) return false;
            uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(str[i + 1]) & 0x3F);
            codepoints.push_back(cp);
            i += 2;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            if (i + 2 >= len) return false;
            uint32_t cp = ((c & 0x0F) << 12) |
                          ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 6) |
                          (static_cast<uint8_t>(str[i + 2]) & 0x3F);
            codepoints.push_back(cp);
            i += 3;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            if (i + 3 >= len) return false;
            uint32_t cp = ((c & 0x07) << 18) |
                          ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 12) |
                          ((static_cast<uint8_t>(str[i + 2]) & 0x3F) << 6) |
                          (static_cast<uint8_t>(str[i + 3]) & 0x3F);
            codepoints.push_back(cp);
            i += 4;
        }
        else
        {
            // Invalid byte, advance 1
            i += 1;
        }
    }
    return true;
}

std::string ArabicShaper::encodeUtf8(const std::vector<uint32_t>& codepoints)
{
    std::string out;
    out.reserve(codepoints.size() * 3);

    for (uint32_t cp : codepoints)
    {
        if (cp <= 0x7F)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp <= 0x7FF)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp <= 0xFFFF)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp <= 0x10FFFF)
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

bool ArabicShaper::isArabicCodepoint(uint32_t cp)
{
    return (cp >= 0x0600 && cp <= 0x06FF) ||
           (cp >= 0x0750 && cp <= 0x077F) ||
           (cp >= 0x08A0 && cp <= 0x08FF) ||
           (cp >= 0xFB50 && cp <= 0xFDFF) ||
           (cp >= 0xFE70 && cp <= 0xFEFF);
}

bool ArabicShaper::isHaraka(uint32_t cp)
{
    return (cp >= 0x064B && cp <= 0x065F) || (cp == 0x0670);
}

bool ArabicShaper::isWhitespace(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\r';
}

bool ArabicShaper::hasArabic(const std::string& utf8Str)
{
    for (size_t i = 0; i < utf8Str.size(); )
    {
        uint8_t c = static_cast<uint8_t>(utf8Str[i]);
        if (c < 0x80)
        {
            i += 1;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            if (i + 1 < utf8Str.size())
            {
                uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(utf8Str[i + 1]) & 0x3F);
                if (isArabicCodepoint(cp)) return true;
            }
            i += 2;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            if (i + 2 < utf8Str.size())
            {
                uint32_t cp = ((c & 0x0F) << 12) |
                              ((static_cast<uint8_t>(utf8Str[i + 1]) & 0x3F) << 6) |
                              (static_cast<uint8_t>(utf8Str[i + 2]) & 0x3F);
                if (isArabicCodepoint(cp)) return true;
            }
            i += 3;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            i += 4;
        }
        else
        {
            i += 1;
        }
    }
    return false;
}

std::vector<uint32_t> ArabicShaper::shapeRun(const std::vector<uint32_t>& cps)
{
    size_t n = cps.size();
    std::vector<uint32_t> shaped;
    shaped.reserve(n);

    auto connectsRightAt = [&](int idx) -> bool {
        if (idx < 0 || idx >= static_cast<int>(n)) return false;
        uint32_t c = cps[idx];
        auto it = s_arabicMap.find(c);
        // Hamza (0x0621) cannot connect right; all other mapped Arabic letters can
        return (it != s_arabicMap.end() && c != 0x0621);
    };

    size_t i = 0;
    bool prevConnected = false;

    while (i < n)
    {
        uint32_t c = cps[i];

        // Skip Harakat (diacritics)
        if (isHaraka(c))
        {
            i += 1;
            continue;
        }

        // Check Lam-Alef ligature
        if (c == 0x0644 && (i + 1 < n))
        {
            uint32_t nextChar = cps[i + 1];
            auto laIt = s_lamAlefMap.find(nextChar);
            if (laIt != s_lamAlefMap.end())
            {
                if (prevConnected)
                    shaped.push_back(laIt->second.final_form);
                else
                    shaped.push_back(laIt->second.isolated);

                prevConnected = false; // Alef never connects to the left
                i += 2;
                continue;
            }
        }

        auto it = s_arabicMap.find(c);
        if (it != s_arabicMap.end())
        {
            const ArabicForms& forms = it->second;

            // Look ahead for next character (skip harakat)
            size_t nextIdx = i + 1;
            while (nextIdx < n && isHaraka(cps[nextIdx]))
            {
                nextIdx += 1;
            }

            bool nextConnected = connectsRightAt(static_cast<int>(nextIdx)) && forms.connects_left;

            if (prevConnected && nextConnected)
            {
                shaped.push_back(forms.medial);
            }
            else if (prevConnected && !nextConnected)
            {
                shaped.push_back(forms.final_form);
            }
            else if (!prevConnected && nextConnected)
            {
                shaped.push_back(forms.initial);
            }
            else
            {
                shaped.push_back(forms.isolated);
            }

            prevConnected = forms.connects_left;
        }
        else
        {
            shaped.push_back(c);
            prevConnected = false;
        }

        i += 1;
    }

    return shaped;
}

static uint32_t mirrorBracket(uint32_t cp)
{
    switch (cp)
    {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        case '<': return '>';
        case '>': return '<';
        case 0x00AB: return 0x00BB; // « <-> »
        case 0x00BB: return 0x00AB;
        default: return cp;
    }
}

std::string ArabicShaper::processLine(const std::string& line)
{
    std::vector<uint32_t> codepoints;
    if (!decodeUtf8(line, codepoints) || codepoints.empty())
        return line;

    // Check if line contains any Arabic
    size_t arabicCount = 0;
    size_t latinCount = 0;
    for (uint32_t cp : codepoints)
    {
        if (isArabicCodepoint(cp))
            arabicCount++;
        else if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))
            latinCount++;
    }

    if (arabicCount == 0)
        return line;

    // Tokenize into segments: Arabic runs vs non-Arabic runs
    // Arabic runs include Arabic letters, diacritics, Arabic punctuation (، ؛ ؟), and whitespace between Arabic words.
    struct Run
    {
        bool isArabic;
        std::vector<uint32_t> text;
    };

    std::vector<Run> runs;
    size_t i = 0;
    size_t n = codepoints.size();

    while (i < n)
    {
        uint32_t cp = codepoints[i];

        bool isAr = isArabicCodepoint(cp) || cp == 0x060C || cp == 0x061B || cp == 0x061F; // Arabic comma, semicolon, question mark
        if (isAr)
        {
            Run run;
            run.isArabic = true;
            while (i < n)
            {
                uint32_t cur = codepoints[i];
                if (isArabicCodepoint(cur) || cur == 0x060C || cur == 0x061B || cur == 0x061F ||
                    cur == ' ' || cur == '\t')
                {
                    // Check if whitespace is followed by another Arabic character
                    if (isWhitespace(cur))
                    {
                        size_t lookahead = i + 1;
                        while (lookahead < n && isWhitespace(codepoints[lookahead]))
                            lookahead++;
                        if (lookahead < n && isArabicCodepoint(codepoints[lookahead]))
                        {
                            run.text.push_back(cur);
                            i++;
                            continue;
                        }
                        else
                        {
                            // Whitespace separates Arabic run from LTR run
                            break;
                        }
                    }
                    run.text.push_back(cur);
                    i++;
                }
                else
                {
                    break;
                }
            }
            runs.push_back(std::move(run));
        }
        else
        {
            Run run;
            run.isArabic = false;
            while (i < n)
            {
                uint32_t cur = codepoints[i];
                if (isArabicCodepoint(cur) || cur == 0x060C || cur == 0x061B || cur == 0x061F)
                {
                    break;
                }
                run.text.push_back(cur);
                i++;
            }
            runs.push_back(std::move(run));
        }
    }

    bool primaryRtl = (arabicCount >= latinCount);

    // Process each run:
    // For Arabic runs: shape, mirror brackets, and reverse for LTR canvas rendering
    for (auto& run : runs)
    {
        if (run.isArabic)
        {
            std::vector<uint32_t> shaped = shapeRun(run.text);
            for (auto& c : shaped)
            {
                c = mirrorBracket(c);
            }
            std::reverse(shaped.begin(), shaped.end());
            run.text = std::move(shaped);
        }
    }

    // If the sentence is predominantly Arabic (RTL), reverse run sequence so Arabic starts on right
    if (primaryRtl && runs.size() > 1)
    {
        std::reverse(runs.begin(), runs.end());
    }

    std::vector<uint32_t> result;
    for (const auto& run : runs)
    {
        result.insert(result.end(), run.text.begin(), run.text.end());
    }

    return encodeUtf8(result);
}

std::string ArabicShaper::shape(const std::string& utf8Str)
{
    if (!hasArabic(utf8Str))
        return utf8Str;

    // Process line by line to preserve newlines
    std::string result;
    std::stringstream ss(utf8Str);
    std::string line;
    bool first = true;

    while (std::getline(ss, line, '\n'))
    {
        if (!first)
            result += '\n';
        first = false;

        result += processLine(line);
    }

    // Preserve trailing newline if input had one
    if (!utf8Str.empty() && utf8Str.back() == '\n')
    {
        result += '\n';
    }

    return result;
}

} // namespace util
