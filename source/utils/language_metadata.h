#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace util {
using LanguageMask = uint32_t;
enum MetadataLanguage : LanguageMask {
    LangRus = 1u << 0, LangEng = 1u << 1, LangSpa = 1u << 2,
    LangFra = 1u << 3, LangGer = 1u << 4, LangIta = 1u << 5,
    LangJpn = 1u << 6, LangChi = 1u << 7, LangChs = 1u << 8,
    LangCht = 1u << 9, LangPor = 1u << 10, LangAra = 1u << 11,
    LangKor = 1u << 12, LangHun = 1u << 13, LangMulti = 1u << 14,
    LangChinese = LangChi | LangChs | LangCht
};

inline std::string_view trimLanguageToken(std::string_view s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

// Callers pass UTF-8 text already case-folded by the application's utility.
// Exact tokens avoid interpreting a title or "no Chinese" as language support.
inline LanguageMask languageToken(std::string_view token) {
    token = trimLanguageToken(token);
    struct Alias { std::string_view text; LanguageMask language; };
    static constexpr Alias aliases[] = {
        {"rus", LangRus}, {"ru", LangRus}, {"russian", LangRus}, {"рус", LangRus}, {"русский", LangRus}, {"русская", LangRus}, {"俄语", LangRus}, {"俄文", LangRus}, {"ruso", LangRus}, {"russe", LangRus}, {"russisch", LangRus}, {"russo", LangRus},
        {"eng", LangEng}, {"en", LangEng}, {"eng-us", LangEng}, {"eng-uk", LangEng}, {"english", LangEng}, {"англ", LangEng}, {"английский", LangEng}, {"английская", LangEng}, {"英语", LangEng}, {"英文", LangEng}, {"inglés", LangEng}, {"anglais", LangEng}, {"englisch", LangEng}, {"inglese", LangEng}, {"inglês", LangEng},
        {"spa", LangSpa}, {"esp", LangSpa}, {"esp-lat", LangSpa}, {"es", LangSpa}, {"spanish", LangSpa}, {"castellano", LangSpa}, {"español", LangSpa}, {"испанский", LangSpa}, {"исп", LangSpa}, {"西班牙语", LangSpa}, {"espagnol", LangSpa}, {"spanisch", LangSpa},
        {"fra", LangFra}, {"fre", LangFra}, {"fr", LangFra}, {"french", LangFra}, {"français", LangFra}, {"французский", LangFra}, {"фран", LangFra}, {"法语", LangFra}, {"francés", LangFra}, {"französisch", LangFra},
        {"ger", LangGer}, {"deu", LangGer}, {"de", LangGer}, {"german", LangGer}, {"deutsch", LangGer}, {"немецкий", LangGer}, {"нем", LangGer}, {"德语", LangGer}, {"alemán", LangGer}, {"allemand", LangGer},
        {"ita", LangIta}, {"it", LangIta}, {"italian", LangIta}, {"italiano", LangIta}, {"итальянский", LangIta}, {"ита", LangIta}, {"意大利语", LangIta},
        {"jpn", LangJpn}, {"jap", LangJpn}, {"ja", LangJpn}, {"japanese", LangJpn}, {"японский", LangJpn}, {"яп", LangJpn}, {"日语", LangJpn}, {"日文", LangJpn}, {"日本語", LangJpn}, {"japonés", LangJpn}, {"japonais", LangJpn},
        {"chi", LangChi}, {"zho", LangChi}, {"zh", LangChi}, {"chinese", LangChi}, {"китайский", LangChi}, {"китайская", LangChi}, {"中文", LangChi}, {"汉语", LangChi}, {"中国語", LangChi}, {"chino", LangChi}, {"chinois", LangChi}, {"chinesisch", LangChi},
        {"chs", LangChs}, {"zh-cn", LangChs}, {"zh-hans", LangChs}, {"simplified chinese", LangChs}, {"chinese (simplified)", LangChs}, {"упрощенный китайский", LangChs}, {"упрощённый китайский", LangChs}, {"简体中文", LangChs}, {"简中", LangChs}, {"简体", LangChs},
        {"cht", LangCht}, {"zh-tw", LangCht}, {"zh-hk", LangCht}, {"zh-hant", LangCht}, {"traditional chinese", LangCht}, {"chinese (traditional)", LangCht}, {"традиционный китайский", LangCht}, {"繁体中文", LangCht}, {"繁體中文", LangCht}, {"繁中", LangCht}, {"繁体", LangCht}, {"繁體", LangCht},
        {"por", LangPor}, {"pt", LangPor}, {"pt-br", LangPor}, {"pt_br", LangPor}, {"por-br", LangPor}, {"portuguese", LangPor}, {"brazilian portuguese", LangPor}, {"português", LangPor}, {"португальский", LangPor}, {"бразильский португальский", LangPor}, {"葡萄牙语", LangPor}, {"巴西葡萄牙语", LangPor},
        {"ara", LangAra}, {"arb", LangAra}, {"ar", LangAra}, {"arabic", LangAra}, {"арабский", LangAra}, {"阿拉伯语", LangAra}, {"العربية", LangAra},
        {"kor", LangKor}, {"ko", LangKor}, {"korean", LangKor}, {"корейский", LangKor}, {"韩语", LangKor}, {"한국어", LangKor},
        {"hun", LangHun}, {"hu", LangHun}, {"hungarian", LangHun}, {"венгерский", LangHun}, {"匈牙利语", LangHun}, {"magyar", LangHun},
        {"multi", LangMulti}, {"multilingual", LangMulti}, {"multilanguage", LangMulti}, {"мульти", LangMulti}, {"多语言", LangMulti}, {"多国语言", LangMulti}, {"多语", LangMulti}
    };
    for (const auto& alias : aliases) if (token == alias.text) return alias.language;
    for (std::string_view prefix : {std::string_view("multi"), std::string_view("мульти")}) {
        if (token.size() > prefix.size() && token.compare(0, prefix.size(), prefix) == 0) {
            std::string_view rest = token.substr(prefix.size());
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '-')) rest.remove_prefix(1);
            if (!rest.empty()) {
                bool allDigits = true;
                for (char c : rest) {
                    if (c < '0' || c > '9') { allDigits = false; break; }
                }
                if (allDigits) return LangMulti;
            }
        }
    }
    return 0;
}

inline LanguageMask metadataLanguageMask(std::string_view lower) {
    std::string normalized(lower);
    for (const std::string_view delimiter : {std::string_view("，"), std::string_view("、"), std::string_view("；"), std::string_view("／"), std::string_view("｜")}) {
        size_t at = 0;
        while ((at = normalized.find(delimiter, at)) != std::string::npos) {
            normalized.replace(at, delimiter.size(), ",");
            ++at;
        }
    }
    for (const auto& pair : {std::pair<std::string_view, std::string_view>{"（", "("}, {"）", ")"}}) {
        size_t at = 0;
        while ((at = normalized.find(pair.first, at)) != std::string::npos) {
            normalized.replace(at, pair.first.size(), pair.second);
            ++at;
        }
    }
    LanguageMask mask = 0;
    const std::string_view text(normalized);
    size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find_first_of("/,;|[]+\n", start);
        const auto chunk = text.substr(start, end == std::string_view::npos ? end : end - start);
        if (const LanguageMask exact = languageToken(chunk)) {
            mask |= exact;
        } else {
            size_t subStart = 0;
            while (subStart < chunk.size()) {
                const auto subEnd = chunk.find_first_of("()", subStart);
                const auto sub = chunk.substr(subStart, subEnd == std::string_view::npos ? subEnd : subEnd - subStart);
                mask |= languageToken(sub);
                if (subEnd == std::string_view::npos) break;
                subStart = subEnd + 1;
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return mask;
}

inline LanguageMask titleLanguageTags(std::string_view title) {
    LanguageMask mask = 0;
    size_t start = 0;
    while ((start = title.find('[', start)) != std::string_view::npos) {
        const auto end = title.find(']', start + 1);
        if (end == std::string_view::npos) break;
        mask |= metadataLanguageMask(title.substr(start + 1, end - start - 1));
        start = end + 1;
    }
    return mask;
}

inline bool metadataIsMulti(LanguageMask mask) {
    if (mask & LangMulti) return true;
    // Generic Chinese plus a script is still one language.
    if (mask & LangChinese) mask = (mask & ~LangChinese) | LangChi;
    return mask && (mask & (mask - 1));
}

inline std::string chineseLanguageLabel(LanguageMask mask, bool chineseUi, bool traditionalUi = false) {
    if (!(mask & LangChinese)) return {};
    if ((mask & LangChs) && !(mask & LangCht)) return chineseUi ? (traditionalUi ? "簡中" : "简中") : "CHS";
    if ((mask & LangCht) && !(mask & LangChs)) return chineseUi ? "繁中" : "CHT";
    return chineseUi ? "中文" : "CHI";
}
} // namespace util
