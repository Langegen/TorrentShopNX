#pragma once

#include <map>
#include <string>
#include <string_view>

namespace util {

inline std::string metadataLocaleKey(std::string_view locale) {
    std::string key;
    for (char c : locale) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        key += c == '_' ? '-' : c;
    }
    return key;
}

inline std::string metadataScriptLocale(const std::string& key) {
    if (key == "zh-cn" || key == "zh-sg" || key.rfind("zh-hans-", 0) == 0) return "zh-hans";
    if (key == "zh-tw" || key == "zh-hk" || key == "zh-mo" || key.rfind("zh-hant-", 0) == 0) return "zh-hant";
    return key;
}

inline std::string localizedMetadataText(const std::map<std::string, std::string>& translations,
                                         const std::string& locale, const std::string& original) {
    const auto hasText = [](const std::string& s) {
        return s.find_first_not_of(" \t\r\n") != std::string::npos;
    };
    if (locale.empty()) return original;
    auto exact = translations.find(locale);
    if (exact != translations.end() && hasText(exact->second)) return exact->second;
    const std::string key = metadataLocaleKey(locale);
    for (const auto& entry : translations)
        if (metadataLocaleKey(entry.first) == key && hasText(entry.second)) return entry.second;
    const std::string script = metadataScriptLocale(key);
    if (script == "zh-hans" || script == "zh-hant") {
        for (const auto& entry : translations)
            if (metadataScriptLocale(metadataLocaleKey(entry.first)) == script && hasText(entry.second))
                return entry.second;
    }
    const std::string base = key.substr(0, key.find('-'));
    for (const auto& entry : translations)
        if (metadataLocaleKey(entry.first) == base && hasText(entry.second)) return entry.second;
    return original;
}

// Only format complete, valid ISO dates. Preserve free-form metadata.
inline std::string formatMetadataDate(const std::string& raw, const std::string& locale) {
    if (raw.size() != 10 || raw[4] != '-' || raw[7] != '-') return raw;
    for (size_t i = 0; i < raw.size(); ++i)
        if (i != 4 && i != 7 && (raw[i] < '0' || raw[i] > '9')) return raw;
    const int y = std::stoi(raw.substr(0, 4)), m = std::stoi(raw.substr(5, 2)), d = std::stoi(raw.substr(8, 2));
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (y < 1 || m < 1 || m > 12 || d < 1) return raw;
    const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    if (d > days[m-1] + (m == 2 && leap ? 1 : 0)) return raw;
    const std::string key = metadataLocaleKey(locale), lang = key.substr(0, key.find('-'));
    const std::string ys = raw.substr(0,4), ms = raw.substr(5,2), ds = raw.substr(8,2);
    if (lang == "zh" || lang == "ja") return ys + "年" + std::to_string(m) + "月" + std::to_string(d) + "日";
    if (lang == "hu") return ys + ". " + ms + ". " + ds + ".";
    if (key == "en-us") return ms + "/" + ds + "/" + ys;
    if (lang == "ru" || lang == "de") return ds + "." + ms + "." + ys;
    if (lang == "en" || lang == "es" || lang == "fr" || lang == "it" || lang == "pt" || lang == "ar")
        return ds + "/" + ms + "/" + ys;
    return raw;
}

inline std::string metadataDateFromTimestamp(const std::string& raw) {
    if (raw.size() < 19 || (raw[10] != ' ' && raw[10] != 'T') || raw[13] != ':' || raw[16] != ':') return raw;
    for (size_t i : {11u, 12u, 14u, 15u, 17u, 18u})
        if (raw[i] < '0' || raw[i] > '9') return raw;
    const int hour = std::stoi(raw.substr(11,2)), minute = std::stoi(raw.substr(14,2)), second = std::stoi(raw.substr(17,2));
    if (hour > 23 || minute > 59 || second > 60) return raw;
    if (raw.size() > 19 && raw[19] != 'Z' && raw[19] != '+' && raw[19] != '-' && raw[19] != '.') return raw;
    return raw.substr(0,10);
}

} // namespace util
