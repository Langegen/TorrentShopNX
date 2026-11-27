// g++ -std=c++17 -Wall -Wextra -I source pctest/test_metadata_locale.cpp -o test_metadata_locale
#include "utils/metadata_locale.h"
#include "catalog/localized_genre_aliases.hpp"
#include <cstdlib>
#include <iostream>

static int failures = 0;
static void check(bool ok, const char* name) {
    if (!ok) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
}

int main() {
    using util::localizedMetadataText;
    using util::formatMetadataDate;
    const std::map<std::string, std::string> text = {
        {"zh-Hans", "简体中文"}, {"zh-Hant", "繁體中文"}, {"en", "English"},
        {"pt", "Português"}, {"pt-BR", "  "}, {"fr", "Français"}
    };
    check(localizedMetadataText(text, "zh-CN", "original") == "简体中文", "Chinese script alias");
    check(localizedMetadataText(text, "zh_HANS", "original") == "简体中文", "locale key normalization");
    check(localizedMetadataText(text, "zh-HK", "original") == "繁體中文", "Traditional Chinese");
    check(localizedMetadataText({{"zh-Hans", "简体"}}, "zh-TW", "original") == "original", "no cross-script fallback");
    check(localizedMetadataText(text, "pt-BR", "original") == "Português", "blank regional translation falls back to language");
    check(localizedMetadataText(text, "en-US", "original") == "English", "base language fallback");
    check(localizedMetadataText(text, "fr-FR", "original") == "Français", "base language French");
    check(localizedMetadataText(text, "ja", "original") == "original", "missing language");
    check(localizedMetadataText(text, "", "original") == "original", "empty locale");
    check(formatMetadataDate("2026-11-09", "zh-Hans") == "2026年11月9日", "Chinese date");
    check(formatMetadataDate("2026-11-09", "ja") == "2026年11月9日", "Japanese date");
    check(formatMetadataDate("2026-11-09", "en-US") == "11/09/2026", "US date");
    check(formatMetadataDate("2026-11-09", "de") == "09.11.2026", "German date");
    check(formatMetadataDate("2026-11-09", "fr") == "09/11/2026", "French date");
    check(formatMetadataDate("2026-11-09", "vi") == "09/11/2026", "Vietnamese date");
    check(formatMetadataDate("2024-02-29", "zh-Hans") == "2024年2月29日", "leap day");
    check(formatMetadataDate("2026-02-29", "zh-Hans") == "2026-02-29", "invalid leap day");
    check(formatMetadataDate("2026-11-09 (port)", "zh-Hans") == "2026-11-09 (port)", "preserve release note");
    check(formatMetadataDate("2026", "zh-Hans") == "2026", "year only");
    check(formatMetadataDate(util::metadataDateFromTimestamp("2026-11-09T21:30:00Z"), "zh-Hans") == "2026年11月9日", "timestamp");
    check(util::metadataDateFromTimestamp("2026-11-09 notes") == "2026-11-09 notes", "preserve non-timestamp");
    const auto& genres = catalog::localizedGenreAliases();
    auto has = [&](const std::string& name, const std::string& id) {
        auto it = genres.find(name);
        if (it == genres.end()) return false;
        for (const auto& value : it->second) if (value == id) return true;
        return false;
    };
    check(has("动作", "action"), "Chinese action");
    check(has("角色扮演 (rpg)", "rpg"), "Chinese RPG");
    check(has("回合制策略", "strategy"), "Chinese turn-based strategy");
    check(has("寻物解谜", "hidden_objects"), "Chinese hidden objects");
    check(has("竞速赛车", "racing"), "Chinese racing");
    check(catalog::normalizeGenreSeparators("动作、冒险，解谜；竞速／体育") == "动作,冒险,解谜,竞速,体育", "full-width separators");
    check(catalog::normalizeGenreSeparators("action / adventure") == "action / adventure", "ASCII separators preserved");
    if (!failures) std::cout << "metadata locale tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
