#include "CollectionsView.hpp"

#include "CatalogView.hpp"
#include "CollectionGamesView.hpp"
#include "EmptyState.hpp"
#include "FavoritesManager.hpp"
#include "FavoritesView.hpp"
#include "GameDetailView.hpp"
#include "ThemeManager.hpp"
#include "style/UiTokens.hpp"
#include "../catalog/game_stats_manager.h"
#include "../utils/i18n_plural.h"
#include "../utils/log.h"
#include "../utils/string_utils.h"

#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <unordered_set>

using namespace brls::literals;

namespace ui {

namespace {

constexpr size_t kPreviewMaxGames = 5;
constexpr int kDebounceMs = 130;
constexpr float kPi = 3.14159265f;

NVGcolor withAlpha(NVGcolor c, unsigned char alpha) {
    return nvgRGBA(static_cast<unsigned char>(std::clamp(c.r, 0.0f, 1.0f) * 255.0f),
                   static_cast<unsigned char>(std::clamp(c.g, 0.0f, 1.0f) * 255.0f),
                   static_cast<unsigned char>(std::clamp(c.b, 0.0f, 1.0f) * 255.0f),
                   alpha);
}

NVGcolor blendColor(NVGcolor a, NVGcolor b, float t) {
    float k = std::clamp(t, 0.0f, 1.0f);
    return nvgRGBAf(a.r + (b.r - a.r) * k,
                    a.g + (b.g - a.g) * k,
                    a.b + (b.b - a.b) * k,
                    a.a + (b.a - a.a) * k);
}

float relativeLuminance(NVGcolor c) {
    auto lin = [](float v) {
        return (v <= 0.04045f) ? (v / 12.92f) : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}

// Keeps sidebar badges & section accents vivid and legible in light mode (avoids pale yellow on white).
NVGcolor sidebarIconColor(const std::string& id, NVGcolor base) {
    if (!style::isLight()) return base;
    if (id == "favorites" || id == "top_100" || relativeLuminance(base) > 0.45f) {
        return nvgRGB(214, 140, 0);
    }
    return base;
}

struct BannerPalette {
    NVGcolor startCol;
    NVGcolor midCol;
    NVGcolor endCol;
};

BannerPalette bannerPaletteFor(const std::string& id, NVGcolor genreColor) {
    auto make = [](int r0, int g0, int b0, int r1, int g1, int b1, int r2, int g2, int b2) -> BannerPalette {
        return {nvgRGB(r0, g0, b0), nvgRGB(r1, g1, b1), nvgRGB(r2, g2, b2)};
    };

    if (id == "all_catalog") {
        if (!style::isLight()) {
            NVGcolor acc = ThemeManager::instance().getAccentColor();
            NVGcolor deep = nvgRGB(10, 22, 34);
            return {
                blendColor(deep, acc, 0.42f),
                blendColor(deep, acc, 0.26f),
                blendColor(nvgRGB(6, 12, 20), acc, 0.14f)
            };
        }
        return make(0, 123, 212,   11, 86, 181,   12, 59, 130);
    }
    if (id == "favorites")           return make(217, 119,   6,  180,  83,   9,  124,  45,  18);
    if (id == "new_release")         return make(  2, 132, 199,   14, 116, 144,   17,  94,  89);
    if (id == "top_100")             return make(184, 115,   0,  140,  74,   8,   89,  40,  11);
    if (id == "action_adventure")    return make(  3, 105, 161,   29,  78, 216,   30,  58, 138);
    if (id == "arcade")              return make(  2, 132, 199,   67,  56, 202,   49,  46, 129);
    if (id == "horror")              return make(147,  51, 234,  107,  33, 168,   59,   7, 100);
    if (id == "metroidvania")        return make(  8, 145, 178,   14, 116, 144,   22,  78,  99);
    if (id == "party_multiplayer")   return make(234,  88,  12,  194,  65,  12,  124,  45,  18);
    if (id == "platformers")         return make(  2, 132, 199,    3, 105, 161,   30,  64, 175);
    if (id == "puzzles")             return make(124,  58, 237,   91,  33, 182,   59,   7, 100);
    if (id == "racing")              return make(220,  38,  38,  185,  28,  28,  127,  29,  29);
    if (id == "roguelike_roguelite") return make(194,  65,  12,  154,  52,  18,   92,  29,  14);
    if (id == "rpg_jrpg")            return make(190,  24,  93,  157,  23,  77,   88,  28, 135);
    if (id == "shooters")            return make(225,  29,  72,  159,  18,  57,   76,   5,  25);
    if (id == "simulation_cozy")     return make( 13, 148, 136,   15, 118, 110,    6,  78,  59);
    if (id == "sports")              return make( 21, 128,  61,   22, 101,  52,   20,  83,  45);
    if (id == "strategy_tactics")    return make( 37,  99, 235,   30,  64, 175,   30,  41,  59);
    if (id == "visual_novels")       return make(192,  38, 211,  134,  25, 143,   74,   4,  78);
    if (id == "ports_homebrew")      return make( 15, 118, 110,   21,  94, 117,   15,  23,  42);

    NVGcolor base = nvgRGBf(genreColor.r * 0.58f, genreColor.g * 0.58f, genreColor.b * 0.58f);
    return {
        base,
        blendColor(base, nvgRGB(18, 24, 38), 0.35f),
        blendColor(base, nvgRGB(10, 14, 24), 0.65f)
    };
}

bool hasStaticBannerArt(const std::string& id) {
    static const std::unordered_set<std::string> kKnownIds = {
        "all_catalog", "favorites", "new_release", "top_100",
        "action_adventure", "arcade", "horror", "metroidvania",
        "party_multiplayer", "platformers", "puzzles", "racing",
        "roguelike_roguelite", "rpg_jrpg", "shooters", "simulation_cozy",
        "sports", "strategy_tactics", "visual_novels", "ports_homebrew"
    };
    return kKnownIds.count(id) > 0;
}

// Draws clean vector glyphs centered at (cx, cy) inside a ~40x40 logical space.
void drawSectionGlyph(NVGcontext* vg, const std::string& id, float cx, float cy, float scale, NVGcolor color) {
    nvgSave(vg);
    nvgTranslate(vg, cx, cy);
    nvgScale(vg, scale, scale);
    nvgFillColor(vg, color);
    nvgStrokeColor(vg, color);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);

    const unsigned char cutoutAlpha = static_cast<unsigned char>(std::clamp(color.a, 0.0f, 1.0f) * 220.0f);
    const NVGcolor cutout = nvgRGBA(16, 22, 30, cutoutAlpha);

    if (id == "all_catalog") {
        float s = 11.0f, g = 5.0f;
        for (int r = 0; r < 2; ++r) {
            for (int c = 0; c < 2; ++c) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, -s - g * 0.5f + c * (s + g),
                                  -s - g * 0.5f + r * (s + g),
                                  s, s, 2.5f);
                nvgFill(vg);
            }
        }
    } else if (id == "favorites") {
        nvgBeginPath(vg);
        for (int i = 0; i < 5; ++i) {
            float a1 = i * (kPi * 2.0f / 5.0f) - kPi * 0.5f;
            float a2 = a1 + kPi / 5.0f;
            float r1 = 18.0f, r2 = 8.0f;
            float x1 = std::cos(a1) * r1, y1 = std::sin(a1) * r1;
            float x2 = std::cos(a2) * r2, y2 = std::sin(a2) * r2;
            if (i == 0) nvgMoveTo(vg, x1, y1);
            else nvgLineTo(vg, x1, y1);
            nvgLineTo(vg, x2, y2);
        }
        nvgClosePath(vg);
        nvgFill(vg);
    } else if (id == "new_release") {
        nvgBeginPath(vg);
        nvgMoveTo(vg, 0.0f, -20.0f);
        nvgQuadTo(vg, 3.5f, -3.5f, 20.0f, 0.0f);
        nvgQuadTo(vg, 3.5f, 3.5f, 0.0f, 20.0f);
        nvgQuadTo(vg, -3.5f, 3.5f, -20.0f, 0.0f);
        nvgQuadTo(vg, -3.5f, -3.5f, 0.0f, -20.0f);
        nvgClosePath(vg);
        nvgFill(vg);
    } else if (id == "top_100") {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, -12.0f, -18.0f, 24.0f, 16.0f, 3.0f);
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRect(vg, -3.5f, -2.0f, 7.0f, 11.0f);
        nvgRoundedRect(vg, -14.0f, 9.0f, 28.0f, 5.0f, 1.5f);
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, -12.0f, -11.0f, 6.0f);
        nvgCircle(vg, 12.0f, -11.0f, 6.0f);
        nvgStrokeWidth(vg, 2.8f);
        nvgStroke(vg);
    } else if (id == "action_adventure") {
        nvgStrokeWidth(vg, 3.4f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, -16.0f, -16.0f); nvgLineTo(vg, 16.0f, 16.0f);
        nvgMoveTo(vg, 16.0f, -16.0f);  nvgLineTo(vg, -16.0f, 16.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, -11.0f, -4.0f); nvgLineTo(vg, -4.0f, -11.0f);
        nvgMoveTo(vg, 11.0f, -4.0f);  nvgLineTo(vg, 4.0f, -11.0f);
        nvgStroke(vg);
    } else if (id == "arcade") {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, -16.0f, 3.0f, 32.0f, 14.0f, 3.5f);
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, 0.0f, 3.0f);
        nvgLineTo(vg, 0.0f, -11.0f);
        nvgStrokeWidth(vg, 3.8f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, 0.0f, -12.0f, 8.0f);
        nvgFill(vg);
    } else if (id == "horror") {
        nvgBeginPath(vg);
        nvgCircle(vg, 0.0f, -3.0f, 14.0f);
        nvgRoundedRect(vg, -8.0f, 4.0f, 16.0f, 12.0f, 2.0f);
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, -5.5f, -3.0f, 3.5f);
        nvgCircle(vg, 5.5f, -3.0f, 3.5f);
        nvgFillColor(vg, cutout);
        nvgFill(vg);
    } else if (id == "metroidvania") {
        nvgStrokeWidth(vg, 3.6f);
        nvgBeginPath(vg);
        nvgCircle(vg, 0.0f, 0.0f, 16.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, 0.0f, 0.0f, 8.0f);
        nvgStroke(vg);
    } else if (id == "party_multiplayer") {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, -16.0f, -11.0f, 20.0f, 13.0f, 3.0f);
        nvgRoundedRect(vg, -4.0f, 1.0f, 20.0f, 13.0f, 3.0f);
        nvgFill(vg);
    } else if (id == "platformers") {
        nvgBeginPath(vg);
        nvgRect(vg, -16.0f, 10.0f, 32.0f, 5.0f);
        nvgFill(vg);
        nvgStrokeWidth(vg, 3.4f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, -13.0f, 3.0f);
        nvgQuadTo(vg, 0.0f, -20.0f, 13.0f, 3.0f);
        nvgStroke(vg);
    } else if (id == "puzzles") {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, -12.0f, -12.0f, 24.0f, 24.0f, 3.0f);
        nvgCircle(vg, 0.0f, -12.0f, 5.0f);
        nvgCircle(vg, 12.0f, 0.0f, 5.0f);
        nvgFill(vg);
    } else if (id == "racing") {
        nvgStrokeWidth(vg, 3.2f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, -14.0f, -17.0f);
        nvgLineTo(vg, -14.0f, 17.0f);
        nvgStroke(vg);
        const float fx = -12.0f, fy = -16.0f, cell = 6.5f;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                if ((r + c) % 2 != 0) continue;
                nvgBeginPath(vg);
                nvgRect(vg, fx + c * cell, fy + r * cell, cell, cell);
                nvgFill(vg);
            }
        }
        nvgBeginPath(vg);
        nvgRect(vg, fx, fy, cell * 4.0f, cell * 4.0f);
        nvgStrokeWidth(vg, 1.6f);
        nvgStroke(vg);
    } else if (id == "roguelike_roguelite") {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, -13.0f, -13.0f, 26.0f, 26.0f, 5.0f);
        nvgFill(vg);
        nvgFillColor(vg, cutout);
        nvgBeginPath(vg);
        nvgCircle(vg, -6.5f, -6.5f, 2.4f);
        nvgCircle(vg, 6.5f, 6.5f, 2.4f);
        nvgCircle(vg, 0.0f, 0.0f, 2.4f);
        nvgCircle(vg, 6.5f, -6.5f, 2.4f);
        nvgCircle(vg, -6.5f, 6.5f, 2.4f);
        nvgFill(vg);
    } else if (id == "rpg_jrpg") {
        nvgBeginPath(vg);
        nvgMoveTo(vg, 0.0f, -18.0f);
        nvgLineTo(vg, 15.0f, -5.5f);
        nvgLineTo(vg, 0.0f, 18.0f);
        nvgLineTo(vg, -15.0f, -5.5f);
        nvgClosePath(vg);
        nvgFill(vg);
    } else if (id == "shooters") {
        nvgStrokeWidth(vg, 3.2f);
        nvgBeginPath(vg);
        nvgCircle(vg, 0.0f, 0.0f, 13.0f);
        nvgMoveTo(vg, -19.0f, 0.0f); nvgLineTo(vg, 19.0f, 0.0f);
        nvgMoveTo(vg, 0.0f, -19.0f); nvgLineTo(vg, 0.0f, 19.0f);
        nvgStroke(vg);
    } else if (id == "simulation_cozy") {
        nvgBeginPath(vg);
        nvgMoveTo(vg, 0.0f, 15.0f);
        nvgQuadTo(vg, -17.0f, -3.5f, 0.0f, -17.0f);
        nvgQuadTo(vg, 17.0f, -3.5f, 0.0f, 15.0f);
        nvgFill(vg);
    } else if (id == "sports") {
        nvgBeginPath(vg);
        nvgCircle(vg, 0.0f, 0.0f, 16.0f);
        nvgFill(vg);
        nvgStrokeColor(vg, cutout);
        nvgStrokeWidth(vg, 2.4f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, -16.0f, 0.0f);
        nvgLineTo(vg, 16.0f, 0.0f);
        nvgMoveTo(vg, -6.0f, -15.0f);
        nvgQuadTo(vg, 6.0f, 0.0f, -6.0f, 15.0f);
        nvgMoveTo(vg, 6.0f, -15.0f);
        nvgQuadTo(vg, -6.0f, 0.0f, 6.0f, 15.0f);
        nvgStroke(vg);
    } else if (id == "strategy_tactics") {
        nvgBeginPath(vg);
        nvgRect(vg, -12.0f, -14.0f, 24.0f, 7.0f);
        nvgRect(vg, -9.0f, -7.0f, 18.0f, 14.0f);
        nvgRect(vg, -13.0f, 7.0f, 26.0f, 8.0f);
        nvgFill(vg);
    } else if (id == "visual_novels") {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, -16.0f, -11.0f, 15.0f, 22.0f, 3.0f);
        nvgRoundedRect(vg, 1.0f, -11.0f, 15.0f, 22.0f, 3.0f);
        nvgFill(vg);
    } else {
        // Generic gamepad ("ports_homebrew" and fallback)
        nvgBeginPath(vg);
        nvgRoundedRect(vg, -17.0f, -10.0f, 34.0f, 20.0f, 7.0f);
        nvgFill(vg);
        nvgFillColor(vg, cutout);
        nvgBeginPath(vg);
        nvgRect(vg, -12.0f, -2.0f, 7.0f, 3.0f);
        nvgRect(vg, -10.0f, -4.0f, 3.0f, 7.0f);
        nvgCircle(vg, 9.0f, -2.0f, 2.2f);
        nvgCircle(vg, 12.5f, 2.0f, 2.2f);
        nvgFill(vg);
    }

    nvgRestore(vg);
}

std::string flattenSpaces(std::string text) {
    for (char& c : text) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == ' ' && !out.empty() && out.back() == ' ') continue;
        out.push_back(c);
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string trimCopy(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Catalog years often look like "2017, март" or "2021, октябрь": extract only the 4-digit year.
std::string extractFourDigitYear(const std::string& raw) {
    if (util::isPlaceholderValue(raw)) return "";
    for (size_t i = 0; i + 4 <= raw.size(); ++i) {
        if ((raw[i] == '1' || raw[i] == '2') &&
            std::isdigit(static_cast<unsigned char>(raw[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(raw[i + 2])) &&
            std::isdigit(static_cast<unsigned char>(raw[i + 3]))) {
            bool prevDigit = (i > 0 && std::isdigit(static_cast<unsigned char>(raw[i - 1])));
            bool nextDigit = (i + 4 < raw.size() && std::isdigit(static_cast<unsigned char>(raw[i + 4])));
            if (!prevDigit && !nextDigit) {
                return raw.substr(i, 4);
            }
        }
    }
    return "";
}

// Base-title deduplication key so alternate editions (e.g. "Zelda: BOTW + DLC" vs "Zelda: BOTW (BOTW) + DLC")
// collapse to the same key while titles like "Super Mario Galaxy 1+2" remain intact.
std::string previewDedupeKey(const std::string& rawTitle) {
    size_t cut = rawTitle.size();
    auto checkCut = [&](size_t pos) {
        if (pos != std::string::npos && pos >= 3 && pos < cut) cut = pos;
    };
    checkCut(rawTitle.find('['));
    checkCut(rawTitle.find('('));
    checkCut(rawTitle.find(" + "));

    std::string base = util::toLowerUtf8(rawTitle.substr(0, cut));
    std::string out;
    out.reserve(base.size());
    bool prevSpace = false;
    for (unsigned char c : base) {
        bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 0x80);
        if (keep) {
            out.push_back(static_cast<char>(c));
            prevSpace = false;
        } else if (!prevSpace && !out.empty()) {
            out.push_back(' ');
            prevSpace = true;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

struct DedupeTracker {
    std::unordered_set<std::string> topicIds;
    std::unordered_set<std::string> covers;
    std::unordered_set<std::string> baseTitles;

    bool accept(const Game& g, bool requireCover = true) {
        if (g.title.empty()) return false;
        if (requireCover && g.cover.empty()) return false;

        if (!g.topic_id.empty() && topicIds.count(g.topic_id)) return false;
        if (!g.cover.empty() && covers.count(g.cover)) return false;
        std::string bk = previewDedupeKey(g.title);
        if (!bk.empty() && baseTitles.count(bk)) return false;

        if (!g.topic_id.empty()) topicIds.insert(g.topic_id);
        if (!g.cover.empty()) covers.insert(g.cover);
        if (!bk.empty()) baseTitles.insert(std::move(bk));
        return true;
    }
};

struct PopularCandidate {
    const Game* game = nullptr;
    double metacritic = 0.0;
    bool has_metacritic = false;
    bool hasStats = false;
    int64_t downloads = 0;
    int32_t seeds = 0;
    size_t origOrder = 0;
};

bool comparePopular(const PopularCandidate& a, const PopularCandidate& b) {
    if (a.hasStats != b.hasStats) return a.hasStats > b.hasStats;
    if (a.downloads != b.downloads) return a.downloads > b.downloads;
    if (a.seeds != b.seeds) return a.seeds > b.seeds;
    return a.origOrder < b.origOrder;
}

std::vector<PreviewGameItem> pickTopPopular(std::vector<PopularCandidate>& candidates) {
    constexpr size_t kSortCap = 36;
    if (candidates.size() > kSortCap) {
        std::partial_sort(candidates.begin(), candidates.begin() + kSortCap, candidates.end(), comparePopular);
        candidates.resize(kSortCap);
    } else {
        std::sort(candidates.begin(), candidates.end(), comparePopular);
    }

    std::vector<PreviewGameItem> out;
    out.reserve(kPreviewMaxGames);
    DedupeTracker dedupe;
    for (const auto& c : candidates) {
        if (!c.game) continue;
        if (!dedupe.accept(*c.game, true)) continue;
        PreviewGameItem item;
        item.game = *c.game;
        item.metacritic = c.metacritic;
        item.has_metacritic = c.has_metacritic;
        out.push_back(std::move(item));
        if (out.size() >= kPreviewMaxGames) break;
    }
    return out;
}

int cachedCollectionCount(const catalog::CollectionInfo& info) {
    if (info.id == "ports_homebrew") {
        auto snap = getCatalogSnapshot();
        if (!snap) return 0;
        int count = 0;
        for (const auto& g : *snap) {
            if (isHomebrewGame(g)) ++count;
        }
        return count;
    }
    std::string body;
    if (!readFileFast(catalog::CollectionsManager::cachePathFor(info.id), body)) return -1;
    try {
        auto j = nlohmann::json::parse(body);
        if (j.is_array()) return static_cast<int>(j.size());
    } catch (...) {}
    return -1;
}

size_t countOfficialGames(const std::shared_ptr<const std::vector<Game>>& snap) {
    if (!snap) return 0;
    size_t count = 0;
    for (const auto& g : *snap) {
        if (!isHomebrewGame(g)) ++count;
    }
    return count;
}

CollectionPreviewData buildFavoritesPreviewData(const std::shared_ptr<const std::vector<Game>>& snap) {
    auto& fm = catalog::FavoritesManager::instance();
    if (snap && !snap->empty()) {
        fm.syncLegacyFavorites(*snap);
    }
    std::vector<Game> favs = fm.getFavorites();

    CollectionPreviewData data;
    data.loaded = true;
    data.failed = false;
    data.totalCount = static_cast<int>(favs.size());

    DedupeTracker dedupe;
    for (auto it = favs.rbegin(); it != favs.rend(); ++it) {
        if (!dedupe.accept(*it, true)) continue;
        PreviewGameItem item;
        item.game = *it;
        data.topGames.push_back(std::move(item));
        if (data.topGames.size() >= kPreviewMaxGames) break;
    }
    if (data.topGames.size() < kPreviewMaxGames) {
        for (auto it = favs.rbegin(); it != favs.rend(); ++it) {
            if (!dedupe.accept(*it, false)) continue;
            PreviewGameItem item;
            item.game = *it;
            data.topGames.push_back(std::move(item));
            if (data.topGames.size() >= kPreviewMaxGames) break;
        }
    }
    return data;
}

brls::Box* createSidebarSectionHeader(const std::string& titleText) {
    brls::Box* box = new brls::Box();
    box->setFocusable(false);
    box->setAxis(brls::Axis::ROW);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setWidthPercentage(100.0f);
    box->setPadding(6.0f, 8.0f, 4.0f, 8.0f);

    brls::Box* bar = new brls::Box();
    bar->setWidth(3.0f);
    bar->setHeight(14.0f);
    bar->setCornerRadius(1.5f);
    bar->setBackgroundColor(ThemeManager::instance().getAccentColor());
    bar->setMarginRight(7.0f);
    box->addView(bar);

    brls::Label* lbl = new brls::Label();
    lbl->setFontSize(14.0f);
    lbl->setLetterSpacing(0.6f);
    lbl->setSingleLine(true);
    lbl->setIsWrapping(false);
    lbl->setMaxLines(1);
    lbl->setAutoAnimate(false);
    lbl->setTextColor(ThemeManager::instance().getTextAccentColor());
    lbl->setGrow(1.0f);
    lbl->setShrink(1.0f);
    lbl->setText(titleText);
    box->addView(lbl);

    return box;
}

bool isDescendantOf(brls::View* view, brls::View* ancestor) {
    for (brls::View* v = view; v != nullptr; v = v->getParent()) {
        if (v == ancestor) return true;
    }
    return false;
}

} // namespace

// ============================================================
// CollectionSidebarRow
// ============================================================
class CollectionSidebarRow : public brls::Box {
public:
    CollectionSidebarRow(const std::string& id,
                         const std::string& title,
                         const std::string& countText,
                         NVGcolor genreColor,
                         bool isMain,
                         std::function<void(const std::string&)> onFocus,
                         std::function<void(const std::string&)> onActivate,
                         std::function<void(const std::string&)> onTouchSelect)
        : id_(id),
          genreColor_(sidebarIconColor(id, genreColor)),
          onFocus_(std::move(onFocus)),
          onActivate_(std::move(onActivate)),
          onTouchSelect_(std::move(onTouchSelect)) {
        this->setFocusable(true);
        this->setHideHighlight(true);
        this->setWidthPercentage(100.0f);
        this->setHeight(isMain ? 48.0f : 46.0f);
        this->setMarginBottom(isMain ? 4.0f : 3.0f);
        this->setPadding(0.0f, 12.0f, 0.0f, 10.0f);
        this->setCornerRadius(11.0f);
        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);

        // 30x30 icon tile spacer
        brls::Box* iconSpacer = new brls::Box();
        iconSpacer->setWidth(30.0f);
        iconSpacer->setHeight(30.0f);
        iconSpacer->setMarginRight(10.0f);
        iconSpacer->setShrink(0.0f);
        this->addView(iconSpacer);

        titleLabel_ = new brls::Label();
        titleLabel_->setFontSize(16.0f);
        titleLabel_->setSingleLine(true);
        titleLabel_->setIsWrapping(false);
        titleLabel_->setMaxLines(1);
        titleLabel_->setAutoAnimate(false);
        titleLabel_->setGrow(1.0f);
        titleLabel_->setShrink(1.0f);
        titleLabel_->setMarginRight(8.0f);
        titleLabel_->setTextColor(style::textPrimary());
        titleLabel_->setText(title);
        this->addView(titleLabel_);

        countLabel_ = new brls::Label();
        countLabel_->setFontSize(14.0f);
        countLabel_->setSingleLine(true);
        countLabel_->setIsWrapping(false);
        countLabel_->setMaxLines(1);
        countLabel_->setAutoAnimate(false);
        countLabel_->setShrink(0.0f);
        countLabel_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        countLabel_->setTextColor(style::textSecondary());
        countLabel_->setText(countText);
        this->addView(countLabel_);

        // Single registration: Borealis registerClickAction binds BUTTON_A + SOUND_CLICK.
        this->registerClickAction([this](brls::View*) {
            if (selected_) {
                if (onActivate_) onActivate_(id_);
            } else {
                if (onTouchSelect_) onTouchSelect_(id_);
            }
            return true;
        });
    }

    void setSelected(bool selected) {
        if (selected_ == selected) return;
        selected_ = selected;
        updateTextColors();
        this->invalidate();
    }

    void setCountText(const std::string& text) {
        if (countLabel_) countLabel_->setText(text);
    }

    void onFocusGained() override {
        Box::onFocusGained();
        if (titleLabel_) titleLabel_->setAnimated(true);
        updateTextColors();
        if (onFocus_) onFocus_(id_);
    }

    void onFocusLost() override {
        Box::onFocusLost();
        if (titleLabel_) titleLabel_->setAnimated(false);
        updateTextColors();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        bool focused = isFocused();
        bool isLight = style::isLight();
        NVGcolor accent = ThemeManager::instance().getAccentColor();

        if (focused) {
            NVGpaint glow = nvgBoxGradient(
                vg, x - 1.0f, y - 1.0f, width + 2.0f, height + 2.0f,
                11.0f, 6.0f,
                this->a(withAlpha(accent, isLight ? 45 : 75)),
                this->a(withAlpha(accent, 0)));
            nvgBeginPath(vg);
            nvgRect(vg, x - 8.0f, y - 8.0f, width + 16.0f, height + 16.0f);
            nvgFillPaint(vg, glow);
            nvgFill(vg);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, 11.0f);
            NVGcolor bg = isLight
                ? nvgRGBA(255, 255, 255, 255)
                : blendColor(ThemeManager::instance().getCardBgColor(), accent, 0.16f);
            nvgFillColor(vg, this->a(bg));
            nvgFill(vg);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, 11.0f);
            nvgStrokeColor(vg, this->a(accent));
            nvgStrokeWidth(vg, 2.2f);
            nvgStroke(vg);
        } else if (selected_) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, 11.0f);
            nvgFillColor(vg, this->a(ThemeManager::instance().getAccentDimColor()));
            nvgFill(vg);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, 11.0f);
            nvgStrokeColor(vg, this->a(withAlpha(accent, isLight ? 105 : 95)));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, x + 2.5f, y + 9.0f, 4.0f, height - 18.0f, 2.0f);
            nvgFillColor(vg, this->a(accent));
            nvgFill(vg);
        }

        // 30x30 rounded icon badge
        const float iconX = x + 10.0f;
        const float iconY = y + (height - 30.0f) * 0.5f;
        if (focused || selected_) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, iconX, iconY, 30.0f, 30.0f, 8.0f);
            nvgFillColor(vg, this->a(genreColor_));
            nvgFill(vg);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, iconX + 1.0f, iconY + 1.0f, 28.0f, 13.0f, 7.0f);
            nvgFillPaint(vg, this->a(nvgLinearGradient(
                vg, iconX, iconY, iconX, iconY + 14.0f,
                nvgRGBA(255, 255, 255, 60), nvgRGBA(255, 255, 255, 0))));
            nvgFill(vg);

            drawSectionGlyph(vg, id_, iconX + 15.0f, iconY + 15.0f, 0.42f, this->a(nvgRGB(255, 255, 255)));
        } else {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, iconX, iconY, 30.0f, 30.0f, 8.0f);
            nvgFillColor(vg, this->a(withAlpha(genreColor_, isLight ? 28 : 42)));
            nvgFill(vg);

            nvgBeginPath(vg);
            nvgRoundedRect(vg, iconX, iconY, 30.0f, 30.0f, 8.0f);
            nvgStrokeColor(vg, this->a(withAlpha(genreColor_, isLight ? 55 : 65)));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);

            drawSectionGlyph(vg, id_, iconX + 15.0f, iconY + 15.0f, 0.42f, this->a(genreColor_));
        }

        updateTextColors();
        Box::draw(vg, x, y, width, height, style, ctx);
    }

private:
    void updateTextColors() {
        bool focused = isFocused();
        if (titleLabel_) {
            titleLabel_->setTextColor((focused || selected_) ? style::textAccent() : style::textPrimary());
        }
        if (countLabel_) {
            countLabel_->setTextColor(focused ? style::textAccent() : style::textSecondary());
        }
    }

    std::string id_;
    NVGcolor genreColor_;
    bool selected_ = false;
    brls::Label* titleLabel_ = nullptr;
    brls::Label* countLabel_ = nullptr;
    std::function<void(const std::string&)> onFocus_;
    std::function<void(const std::string&)> onActivate_;
    std::function<void(const std::string&)> onTouchSelect_;
};

// ============================================================
// HeroOpenButton (crisp high-contrast pill inside Hero Banner)
// ============================================================
class HeroOpenButton : public brls::Box {
public:
    HeroOpenButton(NVGcolor focusTextColor,
                   std::function<void()> onOpenAll,
                   std::function<void()> onBackToSidebar)
        : focusTextColor_(focusTextColor) {
        this->setFocusable(true);
        this->setHideHighlight(true);
        this->setHeight(48.0f);
        this->setPadding(0.0f, 22.0f, 0.0f, 22.0f);
        this->setCornerRadius(12.0f);
        this->setShrink(0.0f);
        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setJustifyContent(brls::JustifyContent::CENTER);

        label_ = new brls::Label();
        label_->setFontSize(style::kFontBody);
        label_->setSingleLine(true);
        label_->setIsWrapping(false);
        label_->setMaxLines(1);
        label_->setAutoAnimate(false);
        label_->setTextColor(nvgRGB(22, 28, 40));
        label_->setText("app/collections/open_all"_i18n + "  \u2192");
        this->addView(label_);

        this->registerClickAction([onOpenAll](brls::View*) {
            if (onOpenAll) onOpenAll();
            return true;
        });

        this->registerAction("hints/back"_i18n, brls::ControllerButton::BUTTON_B, [onBackToSidebar](brls::View*) {
            if (onBackToSidebar) onBackToSidebar();
            return true;
        });
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        float targetScale = isFocused() ? 1.05f : 1.0f;
        scale_ += (targetScale - scale_) * 0.25f;
        float targetGlow = isFocused() ? 1.0f : 0.0f;
        glow_ += (targetGlow - glow_) * 0.25f;

        float cx = x + width * 0.5f;
        float cy = y + height * 0.5f;

        nvgSave(vg);
        nvgTranslate(vg, cx, cy);
        nvgScale(vg, scale_, scale_);
        nvgTranslate(vg, -cx, -cy);

        // Ambient shadow
        NVGpaint shadow = nvgBoxGradient(
            vg, x, y + 3.0f, width, height, 12.0f, 10.0f,
            this->a(nvgRGBA(0, 0, 0, isFocused() ? 95 : 55)),
            this->a(nvgRGBA(0, 0, 0, 0)));
        nvgBeginPath(vg);
        nvgRect(vg, x - 12.0f, y - 6.0f, width + 24.0f, height + 24.0f);
        nvgFillPaint(vg, shadow);
        nvgFill(vg);

        if (glow_ > 0.01f) {
            NVGcolor glowCol = style::isLight() ? nvgRGB(255, 255, 255) : ThemeManager::instance().getAccentColor();
            NVGpaint focusGlow = nvgBoxGradient(
                vg, x - 2.0f, y - 2.0f, width + 4.0f, height + 4.0f, 12.0f, 8.0f,
                this->a(withAlpha(glowCol, static_cast<unsigned char>(130.0f * glow_))),
                this->a(withAlpha(glowCol, 0)));
            nvgBeginPath(vg);
            nvgRect(vg, x - 12.0f, y - 12.0f, width + 24.0f, height + 24.0f);
            nvgFillPaint(vg, focusGlow);
            nvgFill(vg);
        }

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 12.0f);
        nvgFillColor(vg, this->a(isFocused() ? nvgRGB(255, 255, 255) : nvgRGBA(255, 255, 255, 238)));
        nvgFill(vg);

        if (glow_ > 0.01f) {
            NVGcolor ringCol = style::isLight() ? nvgRGB(255, 255, 255) : ThemeManager::instance().getAccentColor();
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, 12.0f);
            nvgStrokeColor(vg, this->a(withAlpha(ringCol, static_cast<unsigned char>(255.0f * glow_))));
            nvgStrokeWidth(vg, 2.8f);
            nvgStroke(vg);
        }

        if (label_) {
            NVGcolor textCol = isFocused()
                ? (style::isLight() ? focusTextColor_ : nvgRGB(15, 22, 32))
                : nvgRGB(22, 28, 40);
            label_->setTextColor(textCol);
        }

        Box::draw(vg, x, y, width, height, style, ctx);
        nvgRestore(vg);
    }

private:
    NVGcolor focusTextColor_;
    brls::Label* label_ = nullptr;
    float scale_ = 1.0f;
    float glow_ = 0.0f;
};

// ============================================================
// BannerScrimOverlay (gradient blend + watermark + top gloss)
// ============================================================
class BannerScrimOverlay : public brls::View {
public:
    BannerScrimOverlay(const std::string& id, BannerPalette pal, bool hasHeroArt)
        : id_(id), pal_(pal), hasHeroArt_(hasHeroArt) {
        this->setFocusable(false);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        if (hasHeroArt_) {
            // Smooth horizontal blend from 100% opaque banner color across the artwork's left edge
            float fadeStart = std::max(width * 0.39f, width - 468.0f);
            float fadeEnd = width * 0.78f;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, 18.0f);
            nvgFillPaint(vg, this->a(nvgLinearGradient(
                vg, x + fadeStart, y, x + fadeEnd, y,
                withAlpha(pal_.startCol, 255),
                withAlpha(pal_.endCol, 24))));
            nvgFill(vg);

            // Subtle dark bottom vignette for crisp readability
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, 18.0f);
            nvgFillPaint(vg, this->a(nvgLinearGradient(
                vg, x, y + height * 0.45f, x, y + height,
                nvgRGBA(0, 0, 0, 0), nvgRGBA(0, 0, 0, 48))));
            nvgFill(vg);
        } else {
            // Atmospheric radial spotlight + decorative glyph watermark when no static artwork is present
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, 18.0f);
            nvgFillPaint(vg, this->a(nvgRadialGradient(
                vg, x + width * 0.68f, y + height * 0.45f, 20.0f, width * 0.55f,
                nvgRGBA(255, 255, 255, 38), nvgRGBA(255, 255, 255, 0))));
            nvgFill(vg);

            nvgSave(vg);
            nvgIntersectScissor(vg, x, y, width, height);
            drawSectionGlyph(vg, id_, x + width * 0.63f, y + height * 0.52f, 2.5f,
                             this->a(nvgRGBA(255, 255, 255, 28)));
            nvgRestore(vg);
        }

        // Top glass sheen
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 1.0f, y + 1.0f, width - 2.0f, height * 0.42f, 17.0f);
        nvgFillPaint(vg, this->a(nvgLinearGradient(
            vg, x, y, x, y + height * 0.42f,
            nvgRGBA(255, 255, 255, hasHeroArt_ ? 30 : 40), nvgRGBA(255, 255, 255, 0))));
        nvgFill(vg);

        // Subtle inner light rim
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 0.5f, y + 0.5f, width - 1.0f, height - 1.0f, 18.0f);
        nvgStrokeColor(vg, this->a(nvgRGBA(255, 255, 255, 62)));
        nvgStrokeWidth(vg, 1.2f);
        nvgStroke(vg);
    }

private:
    std::string id_;
    BannerPalette pal_;
    bool hasHeroArt_ = false;
};

// ============================================================
// CollectionPreviewBanner (Rich 178 px Storefront Hero Banner)
// ============================================================
class CollectionPreviewBanner : public brls::Box {
public:
    CollectionPreviewBanner(const std::string& id,
                            const std::string& title,
                            const std::string& desc,
                            const std::string& countText,
                            int maxMetacritic,
                            NVGcolor genreColor,
                            std::function<void()> onOpenAll,
                            std::function<void()> onBackToSidebar)
        : pal_(bannerPaletteFor(id, genreColor)) {
        this->setFocusable(false);
        this->setWidthPercentage(100.0f);
        this->setHeight(178.0f);
        this->setCornerRadius(18.0f);
        this->setAxis(brls::Axis::COLUMN);
        this->setMarginBottom(16.0f);

        const bool hasHeroArt = hasStaticBannerArt(id);
        if (hasHeroArt) {
            brls::Image* heroImg = new brls::Image();
            heroImg->setPositionType(brls::PositionType::ABSOLUTE);
            heroImg->setPositionTop(0.0f);
            heroImg->setPositionRight(0.0f);
            heroImg->setWidth(470.0f);
            heroImg->setHeight(178.0f);
            heroImg->setCornerRadius(18.0f);
            heroImg->setScalingType(brls::ImageScalingType::FILL);
            heroImg->setImageFromRes("img/collections/" + id + ".jpg");
            this->addView(heroImg);
        }

        auto* scrim = new BannerScrimOverlay(id, pal_, hasHeroArt);
        scrim->setPositionType(brls::PositionType::ABSOLUTE);
        scrim->setPositionTop(0.0f);
        scrim->setPositionLeft(0.0f);
        scrim->setWidthPercentage(100.0f);
        scrim->setHeightPercentage(100.0f);
        this->addView(scrim);

        // Foreground content row (explicit 178 px height so Yoga does not resolve 100% against rightPanel)
        brls::Box* fgRow = new brls::Box();
        fgRow->setFocusable(false);
        fgRow->setWidthPercentage(100.0f);
        fgRow->setHeight(178.0f);
        fgRow->setPadding(22.0f, 26.0f, 22.0f, 26.0f);
        fgRow->setAxis(brls::Axis::ROW);
        fgRow->setAlignItems(brls::AlignItems::CENTER);
        fgRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);

        brls::Box* infoCol = new brls::Box();
        infoCol->setAxis(brls::Axis::COLUMN);
        infoCol->setJustifyContent(brls::JustifyContent::CENTER);
        infoCol->setGrow(1.0f);
        infoCol->setShrink(1.0f);
        infoCol->setMarginRight(24.0f);

        // Top badges row
        brls::Box* pillsRow = new brls::Box();
        pillsRow->setAxis(brls::Axis::ROW);
        pillsRow->setAlignItems(brls::AlignItems::CENTER);

        if (!countText.empty()) {
            brls::Box* chip = new brls::Box();
            chip->setCornerRadius(7.0f);
            chip->setPadding(3.0f, 11.0f, 3.0f, 11.0f);
            chip->setBackgroundColor(nvgRGBA(255, 255, 255, 48));
            chip->setBorderThickness(1.0f);
            chip->setBorderColor(nvgRGBA(255, 255, 255, 72));

            brls::Label* chipLbl = new brls::Label();
            chipLbl->setFontSize(style::kFontCaption);
            chipLbl->setSingleLine(true);
            chipLbl->setIsWrapping(false);
            chipLbl->setMaxLines(1);
            chipLbl->setAutoAnimate(false);
            chipLbl->setTextColor(nvgRGB(255, 255, 255));
            chipLbl->setText(countText);
            chip->addView(chipLbl);
            pillsRow->addView(chip);
        }

        if (maxMetacritic > 0) {
            brls::Box* metaChip = new brls::Box();
            metaChip->setCornerRadius(7.0f);
            metaChip->setPadding(3.0f, 11.0f, 3.0f, 11.0f);
            metaChip->setMarginLeft(8.0f);
            metaChip->setBackgroundColor(nvgRGBA(255, 210, 60, 68));
            metaChip->setBorderThickness(1.0f);
            metaChip->setBorderColor(nvgRGBA(255, 224, 110, 120));

            brls::Label* metaLbl = new brls::Label();
            metaLbl->setFontSize(style::kFontCaption);
            metaLbl->setSingleLine(true);
            metaLbl->setIsWrapping(false);
            metaLbl->setMaxLines(1);
            metaLbl->setAutoAnimate(false);
            metaLbl->setTextColor(nvgRGB(255, 246, 204));
            metaLbl->setText("\u2605 Metacritic " + std::to_string(maxMetacritic));
            metaChip->addView(metaLbl);
            pillsRow->addView(metaChip);
        }

        if (!pillsRow->getChildren().empty()) {
            infoCol->addView(pillsRow);
        }

        brls::Label* titleLbl = new brls::Label();
        titleLbl->setFontSize(30.0f);
        titleLbl->setSingleLine(true);
        titleLbl->setIsWrapping(false);
        titleLbl->setMaxLines(1);
        titleLbl->setAutoAnimate(false);
        titleLbl->setTextColor(nvgRGB(255, 255, 255));
        titleLbl->setMarginTop(10.0f);
        titleLbl->setText(title);
        infoCol->addView(titleLbl);

        std::string cleanDesc = flattenSpaces(desc);
        if (!cleanDesc.empty()) {
            brls::Label* descLbl = new brls::Label();
            descLbl->setFontSize(style::kFontBody);
            descLbl->setMaxLines(2);
            descLbl->setLineHeight(1.25f);
            descLbl->setAutoAnimate(false);
            descLbl->setTextColor(nvgRGBA(255, 255, 255, 225));
            descLbl->setMarginTop(5.0f);
            descLbl->setText(cleanDesc);
            infoCol->addView(descLbl);
        }

        fgRow->addView(infoCol);

        openBtn_ = new HeroOpenButton(pal_.startCol, std::move(onOpenAll), std::move(onBackToSidebar));
        fgRow->addView(openBtn_);

        this->addView(fgRow);
    }

    brls::Box* button() const { return openBtn_; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        // Soft ambient drop shadow
        NVGpaint shadow = nvgBoxGradient(
            vg, x + 2.0f, y + 6.0f, width - 4.0f, height, 18.0f, 18.0f,
            this->a(nvgRGBA(12, 18, 32, style::isLight() ? 48 : 110)),
            this->a(nvgRGBA(12, 18, 32, 0)));
        nvgBeginPath(vg);
        nvgRect(vg, x - 18.0f, y - 6.0f, width + 36.0f, height + 36.0f);
        nvgFillPaint(vg, shadow);
        nvgFill(vg);

        // Rich base gradient
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 18.0f);
        nvgFillPaint(vg, this->a(nvgLinearGradient(
            vg, x, y, x + width, y + height, pal_.startCol, pal_.endCol)));
        nvgFill(vg);

        Box::draw(vg, x, y, width, height, style, ctx);
    }

private:
    BannerPalette pal_;
    HeroOpenButton* openBtn_ = nullptr;
};

// ============================================================
// PreviewGameCard
// ============================================================
class PreviewGameCard : public brls::Box {
public:
    PreviewGameCard(const PreviewGameItem& item,
                    int colIndex,
                    const std::shared_ptr<bool>& imageToken,
                    std::function<void()> onFavoriteToggled,
                    std::function<void()> onBackToSidebar)
        : game_(item.game) {
        this->setFocusable(true);
        this->setHideHighlight(true);
        this->setWidth(158.0f);
        this->setHeight(302.0f);
        this->setPadding(6.0f);
        this->setCornerRadius(12.0f);
        this->setAxis(brls::Axis::COLUMN);

        brls::Box* coverBox = new brls::Box();
        coverBox->setWidth(146.0f);
        coverBox->setHeight(230.0f);
        coverBox->setCornerRadius(style::kRadiusCover);
        coverBox->setBackgroundColor(style::isLight()
            ? nvgRGBA(224, 230, 238, 255)
            : nvgRGBA(255, 255, 255, 24));
        coverBox->setBorderThickness(1.0f);
        coverBox->setBorderColor(style::isLight()
            ? nvgRGBA(15, 23, 42, 22)
            : nvgRGBA(255, 255, 255, 32));
        coverBox->setAlignItems(brls::AlignItems::CENTER);
        coverBox->setJustifyContent(brls::JustifyContent::CENTER);

        brls::Image* coverImg = new brls::Image();
        coverImg->setWidth(146.0f);
        coverImg->setHeight(230.0f);
        coverImg->setCornerRadius(style::kRadiusCover);
        coverImg->setScalingType(brls::ImageScalingType::FILL);
        setImageFromHTTPS(coverImg, game_.cover, imageToken, "romfs:/img/cover_blank.png",
                          false, "", 0, colIndex, 100000 - colIndex);
        coverBox->addView(coverImg);

        if (item.has_metacritic) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), " %.0f ", item.metacritic);
            brls::Label* metaLbl = new brls::Label();
            metaLbl->setFontSize(13.0f);
            metaLbl->setSingleLine(true);
            metaLbl->setIsWrapping(false);
            metaLbl->setAutoAnimate(false);
            metaLbl->setTextColor(nvgRGB(255, 255, 255));
            metaLbl->setCornerRadius(6.0f);
            NVGcolor mcColor = item.metacritic >= 75.0 ? nvgRGB(0x66, 0xCC, 0x33)
                             : item.metacritic >= 50.0 ? nvgRGB(0xFF, 0xCC, 0x33)
                                                       : nvgRGB(0xFF, 0x33, 0x00);
            metaLbl->setBackgroundColor(mcColor);
            metaLbl->setPositionType(brls::PositionType::ABSOLUTE);
            metaLbl->setPositionTop(5.0f);
            metaLbl->setPositionRight(5.0f);
            metaLbl->setText(buf);
            coverBox->addView(metaLbl);
        }

        GameLangBadge langBadge = getGameLangBadge(game_.interface_lang, brls::Application::getLocale());
        if (!langBadge.text.empty()) {
            brls::Label* langLbl = new brls::Label();
            langLbl->setFontSize(13.0f);
            langLbl->setSingleLine(true);
            langLbl->setIsWrapping(false);
            langLbl->setAutoAnimate(false);
            langLbl->setTextColor(nvgRGB(255, 255, 255));
            langLbl->setBackgroundColor(langBadge.color);
            langLbl->setCornerRadius(5.0f);
            langLbl->setPositionType(brls::PositionType::ABSOLUTE);
            langLbl->setPositionBottom(5.0f);
            langLbl->setPositionLeft(5.0f);
            langLbl->setText("  " + langBadge.text + "  ");
            coverBox->addView(langLbl);
        }

        this->addView(coverBox);

        // Configure singleLine / maxLines / width BEFORE setText and without fixed height so it never wraps.
        titleLabel_ = new brls::Label();
        titleLabel_->setFontSize(style::kFontSub);
        titleLabel_->setSingleLine(true);
        titleLabel_->setIsWrapping(false);
        titleLabel_->setMaxLines(1);
        titleLabel_->setAutoAnimate(false);
        titleLabel_->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        titleLabel_->setWidth(146.0f);
        titleLabel_->setMaxWidth(146.0f);
        titleLabel_->setMarginTop(7.0f);
        titleLabel_->setTextColor(style::textPrimary());
        titleLabel_->setText(cleanTitle(game_.title));
        this->addView(titleLabel_);

        brls::Box* metaRow = new brls::Box();
        metaRow->setWidth(146.0f);
        metaRow->setAxis(brls::Axis::ROW);
        metaRow->setAlignItems(brls::AlignItems::CENTER);
        metaRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        metaRow->setMarginTop(3.0f);

        std::string sizeStr = util::isPlaceholderValue(game_.size) ? "" : game_.size;
        brls::Label* sizeLbl = new brls::Label();
        sizeLbl->setFontSize(style::kFontCaption);
        sizeLbl->setSingleLine(true);
        sizeLbl->setIsWrapping(false);
        sizeLbl->setMaxLines(1);
        sizeLbl->setAutoAnimate(false);
        sizeLbl->setShrink(0.0f);
        sizeLbl->setTextColor(style::textSecondary());
        sizeLbl->setText(sizeStr);
        metaRow->addView(sizeLbl);

        std::string yearStr = extractFourDigitYear(game_.year);
        if (!yearStr.empty()) {
            brls::Label* yearLbl = new brls::Label();
            yearLbl->setFontSize(style::kFontCaption);
            yearLbl->setSingleLine(true);
            yearLbl->setIsWrapping(false);
            yearLbl->setMaxLines(1);
            yearLbl->setAutoAnimate(false);
            yearLbl->setShrink(0.0f);
            yearLbl->setTextColor(style::textSecondary());
            yearLbl->setText(yearStr);
            metaRow->addView(yearLbl);
        }

        this->addView(metaRow);

        Game gameCopy = game_;
        this->registerClickAction([gameCopy](brls::View*) {
            brls::Application::pushActivity(new GameDetailView(gameCopy));
            return true;
        });

        this->registerAction("app/actions/toggle_favorite"_i18n, brls::ControllerButton::BUTTON_Y,
                             [gameCopy, onFavoriteToggled](brls::View*) {
            bool fav = catalog::FavoritesManager::instance().toggleFavorite(gameCopy);
            brls::Application::notify(fav ? "app/favorites/added"_i18n : "app/favorites/removed"_i18n);
            if (onFavoriteToggled) onFavoriteToggled();
            return true;
        });

        this->registerAction("hints/back"_i18n, brls::ControllerButton::BUTTON_B, [onBackToSidebar](brls::View*) {
            if (onBackToSidebar) onBackToSidebar();
            return true;
        });
    }

    void onFocusGained() override {
        Box::onFocusGained();
        if (titleLabel_) {
            titleLabel_->setAnimated(true);
            titleLabel_->setTextColor(style::textAccent());
        }
    }

    void onFocusLost() override {
        Box::onFocusLost();
        if (titleLabel_) {
            titleLabel_->setAnimated(false);
            titleLabel_->setTextColor(style::textPrimary());
        }
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        float targetScale = isFocused() ? 1.035f : 1.0f;
        scale_ += (targetScale - scale_) * 0.24f;
        float targetGlow = isFocused() ? 1.0f : 0.0f;
        glow_ += (targetGlow - glow_) * 0.24f;

        float cx = x + width * 0.5f;
        float cy = y + height * 0.5f;

        nvgSave(vg);
        nvgTranslate(vg, cx, cy);
        nvgScale(vg, scale_, scale_);
        nvgTranslate(vg, -cx, -cy);

        // Ambient elevation shadow even when unfocused
        NVGpaint baseShadow = nvgBoxGradient(
            vg, x + 1.0f, y + 4.0f, width - 2.0f, height, 12.0f, 12.0f,
            this->a(nvgRGBA(15, 23, 42, style::isLight() ? 28 : 95)),
            this->a(nvgRGBA(15, 23, 42, 0)));
        nvgBeginPath(vg);
        nvgRect(vg, x - 12.0f, y - 4.0f, width + 24.0f, height + 24.0f);
        nvgFillPaint(vg, baseShadow);
        nvgFill(vg);

        NVGcolor accent = ThemeManager::instance().getAccentColor();
        if (glow_ > 0.01f) {
            NVGpaint glowPaint = nvgBoxGradient(
                vg, x - 2.0f, y - 2.0f, width + 4.0f, height + 4.0f,
                12.0f, 8.0f,
                this->a(withAlpha(accent, static_cast<unsigned char>(95.0f * glow_))),
                this->a(withAlpha(accent, 0)));
            nvgBeginPath(vg);
            nvgRect(vg, x - 12.0f, y - 12.0f, width + 24.0f, height + 24.0f);
            nvgFillPaint(vg, glowPaint);
            nvgFill(vg);
        }

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 12.0f);
        nvgFillColor(vg, this->a(style::cardBg()));
        nvgFill(vg);

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 12.0f);
        if (glow_ > 0.01f) {
            nvgStrokeColor(vg, this->a(withAlpha(accent, static_cast<unsigned char>(255.0f * glow_))));
            nvgStrokeWidth(vg, 2.5f);
        } else {
            nvgStrokeColor(vg, this->a(style::cardBorder()));
            nvgStrokeWidth(vg, 1.0f);
        }
        nvgStroke(vg);

        Box::draw(vg, x, y, width, height, style, ctx);
        nvgRestore(vg);
    }

private:
    Game game_;
    brls::Label* titleLabel_ = nullptr;
    float scale_ = 1.0f;
    float glow_ = 0.0f;
};

// ============================================================
// CollectionsView
// ============================================================
CollectionsView::CollectionsView()
    : alive_flag_(std::make_shared<std::atomic<bool>>(true)),
      previewImageToken_(std::make_shared<bool>(true)) {
}

CollectionsView::~CollectionsView() {
    *alive_flag_ = false;
    if (previewImageToken_) {
        *previewImageToken_ = false;
    }
    ThemeManager::instance().unsubscribe(themeSub_);
}

void CollectionsView::onContentAvailable() {
    if (sidebarScroll) {
        sidebarScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    }

    buildEntriesList();

    // Seed favorites & known cache counts right away to avoid initial flicker
    auto snap = getCatalogSnapshot();
    previewCache_["favorites"] = buildFavoritesPreviewData(snap);
    {
        CollectionPreviewData allSeed;
        allSeed.loaded = false;
        allSeed.totalCount = static_cast<int>(countOfficialGames(snap));
        previewCache_["all_catalog"] = std::move(allSeed);
    }
    for (const auto& entry : entries_) {
        if (!entry.isCollection) continue;
        int cached = cachedCollectionCount(entry.collectionInfo);
        if (cached >= 0) {
            CollectionPreviewData seed;
            seed.loaded = false;
            seed.totalCount = cached;
            previewCache_[entry.id] = std::move(seed);
        }
    }

    this->registerAction("app/actions/search"_i18n, brls::ControllerButton::BUTTON_X, [this](brls::View*) {
        openQuickSearch();
        return true;
    });

    if (headerTitle) {
        headerTitle->addGestureRecognizer(new brls::TapGestureRecognizer(headerTitle, [this]() {
            openQuickSearch();
        }));
    }

    applyPanelTheme();
    updateHeaderStats();
    rebuildSidebar();
    if (!previewRoot_) {
        rebuildPreviewPanel(false);
    }
    startBackgroundPreload();

    themeSub_ = ThemeManager::instance().subscribe([this, flag = alive_flag_]() {
        if (!flag->load()) return;
        applyPanelTheme();
        updateHeaderStats();
        rebuildSidebar();
        rebuildPreviewPanel(false);
    });
}

void CollectionsView::willAppear(bool resetState) {
    brls::Activity::willAppear(resetState);

    refreshFavoritesPreview(selectedId_ == "favorites");
    updateHeaderStats();

    auto snap = getCatalogSnapshot();
    int official = static_cast<int>(countOfficialGames(snap));
    auto itAll = previewCache_.find("all_catalog");
    if (itAll != previewCache_.end() && itAll->second.totalCount != official) {
        itAll->second.totalCount = official;
        auto itRow = sidebarRows_.find("all_catalog");
        if (itRow != sidebarRows_.end() && itRow->second) {
            itRow->second->setCountText(std::to_string(official));
        }
    }

    if (brls::Application::getCurrentFocus() == nullptr) {
        returnFocusToSidebar();
    }
}

void CollectionsView::applyPanelTheme() {
    if (!leftPanel) return;
    bool isLight = style::isLight();
    leftPanel->setBackgroundColor(isLight ? nvgRGBA(255, 255, 255, 225) : nvgRGBA(12, 24, 29, 175));
    leftPanel->setBorderThickness(1.0f);
    leftPanel->setBorderColor(style::cardBorder());
}

void CollectionsView::updateHeaderStats() {
    if (!statsHint) return;
    auto snap = getCatalogSnapshot();
    size_t officialCount = countOfficialGames(snap);
    statsHint->setText(util::pluralize("app/plural/games", static_cast<long long>(officialCount)));
}

void CollectionsView::buildEntriesList() {
    entries_.clear();
    const auto& collections = catalog::CollectionsManager::instance().collections();

    catalog::CollectionInfo newReleaseInfo{"new_release", "app/collections/item_new_release_name"_i18n, "app/collections/item_new_release_desc"_i18n};
    catalog::CollectionInfo top100Info{"top_100", "app/collections/item_top_100_name"_i18n, "app/collections/item_top_100_desc"_i18n};
    for (const auto& c : collections) {
        if (c.id == "new_release") newReleaseInfo = c;
        if (c.id == "top_100") top100Info = c;
    }

    SidebarEntryMeta allEntry;
    allEntry.id = "all_catalog";
    allEntry.title = "app/collections/all_catalog"_i18n;
    allEntry.description = "app/collections/all_catalog_desc"_i18n;
    allEntry.isMain = true;
    entries_.push_back(std::move(allEntry));

    SidebarEntryMeta favEntry;
    favEntry.id = "favorites";
    favEntry.title = "app/collections/favorites"_i18n;
    favEntry.description = "app/collections/favorites_desc"_i18n;
    favEntry.isMain = true;
    entries_.push_back(std::move(favEntry));

    SidebarEntryMeta nrEntry;
    nrEntry.id = "new_release";
    nrEntry.title = newReleaseInfo.getName();
    nrEntry.description = newReleaseInfo.getDescription();
    nrEntry.isMain = true;
    nrEntry.isCollection = true;
    nrEntry.collectionInfo = newReleaseInfo;
    entries_.push_back(std::move(nrEntry));

    SidebarEntryMeta topEntry;
    topEntry.id = "top_100";
    topEntry.title = top100Info.getName();
    topEntry.description = top100Info.getDescription();
    topEntry.isMain = true;
    topEntry.isCollection = true;
    topEntry.collectionInfo = top100Info;
    entries_.push_back(std::move(topEntry));

    for (const auto& c : collections) {
        if (c.id == "new_release" || c.id == "top_100") continue;
        SidebarEntryMeta gEntry;
        gEntry.id = c.id;
        gEntry.title = c.getName();
        gEntry.description = c.getDescription();
        gEntry.isMain = false;
        gEntry.isCollection = true;
        gEntry.collectionInfo = c;
        entries_.push_back(std::move(gEntry));
    }
}

const SidebarEntryMeta* CollectionsView::findEntry(const std::string& id) const {
    for (const auto& e : entries_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

void CollectionsView::rebuildSidebar() {
    if (!sidebarList) return;

    bool hadFocusInSidebar = isDescendantOf(brls::Application::getCurrentFocus(), leftPanel);
    if (hadFocusInSidebar) {
        brls::Application::giveFocus(nullptr);
    }

    sidebarList->clearViews();
    sidebarRows_.clear();
    sidebarRowOrder_.clear();

    sidebarList->addView(createSidebarSectionHeader("app/collections/section_main"_i18n));

    bool addedGenresHeader = false;
    for (const auto& entry : entries_) {
        if (!entry.isMain && !addedGenresHeader) {
            addedGenresHeader = true;
            brls::Box* sep = new brls::Box();
            sep->setFocusable(false);
            sep->setWidthPercentage(100.0f);
            sep->setHeight(1.0f);
            sep->setMarginTop(6.0f);
            sep->setMarginBottom(6.0f);
            sep->setBackgroundColor(style::cardBorder());
            sidebarList->addView(sep);
            sidebarList->addView(createSidebarSectionHeader("app/collections/section_genres_short"_i18n));
        }

        std::string countStr = "\u2026";
        auto itCache = previewCache_.find(entry.id);
        if (itCache != previewCache_.end() && itCache->second.totalCount >= 0) {
            countStr = std::to_string(itCache->second.totalCount);
        }

        auto* row = new CollectionSidebarRow(
            entry.id,
            entry.title,
            countStr,
            ThemeManager::instance().getGenreColor(entry.id),
            entry.isMain,
            [this](const std::string& id) { selectItem(id, false); },
            [this](const std::string& id) { openSection(id); },
            [this](const std::string& id) {
                selectItem(id, true);
                auto it = sidebarRows_.find(id);
                if (it != sidebarRows_.end() && it->second) {
                    brls::Application::giveFocus(it->second);
                }
            }
        );
        if (entry.id == selectedId_) {
            row->setSelected(true);
        }
        sidebarRows_[entry.id] = row;
        sidebarRowOrder_.push_back(row);
        sidebarList->addView(row);
    }

    updateNavigationRoutes();

    if (hadFocusInSidebar || brls::Application::getCurrentFocus() == nullptr) {
        auto it = sidebarRows_.find(selectedId_);
        if (it != sidebarRows_.end() && it->second) {
            brls::Application::giveFocus(it->second);
        } else if (!sidebarRowOrder_.empty()) {
            brls::Application::giveFocus(sidebarRowOrder_[0]);
        }
    }
}

void CollectionsView::selectItem(const std::string& id, bool immediate) {
    const bool changed = (selectedId_ != id);
    if (!changed && !immediate && previewRoot_ != nullptr) {
        return;
    }

    if (changed) {
        auto itOld = sidebarRows_.find(selectedId_);
        if (itOld != sidebarRows_.end() && itOld->second) {
            itOld->second->setSelected(false);
        }
        selectedId_ = id;
        auto itNew = sidebarRows_.find(selectedId_);
        if (itNew != sidebarRows_.end() && itNew->second) {
            itNew->second->setSelected(true);
        }
    }

    {
        std::lock_guard<std::mutex> lock(priorityMutex_);
        priorityCollectionId_ = id;
    }

    const auto now = std::chrono::steady_clock::now();
    if (immediate || (now - lastPreviewRebuild_) >= std::chrono::milliseconds(kDebounceMs)) {
        ++previewDebounceGen_;
        lastPreviewRebuild_ = now;
        rebuildPreviewPanel(true);
        return;
    }

    uint64_t gen = ++previewDebounceGen_;
    auto flag = alive_flag_;
    brls::delay(kDebounceMs, [this, flag, gen, id]() {
        if (!flag->load()) return;
        if (gen != previewDebounceGen_ || selectedId_ != id) return;
        lastPreviewRebuild_ = std::chrono::steady_clock::now();
        rebuildPreviewPanel(true);
    });
}

void CollectionsView::returnFocusToSidebar() {
    auto it = sidebarRows_.find(selectedId_);
    if (it != sidebarRows_.end() && it->second) {
        brls::Application::giveFocus(it->second);
    } else if (!sidebarRowOrder_.empty()) {
        brls::Application::giveFocus(sidebarRowOrder_[0]);
    }
}

void CollectionsView::rebuildPreviewPanel(bool animateFade) {
    if (!rightPanel) return;

    const SidebarEntryMeta* meta = findEntry(selectedId_);
    if (!meta) return;

    if (previewImageToken_) {
        *previewImageToken_ = false;
    }
    previewImageToken_ = std::make_shared<bool>(true);

    // Preserve focus safely before destroying rightPanel children
    brls::View* curFocus = brls::Application::getCurrentFocus();
    bool focusWasInRight = isDescendantOf(curFocus, rightPanel);
    int focusedCardIdx = -1;
    bool focusWasOnOpenBtn = (openAllBtn_ != nullptr && curFocus == openAllBtn_);
    if (focusWasInRight && !focusWasOnOpenBtn) {
        for (size_t i = 0; i < previewCards_.size(); ++i) {
            if (curFocus == previewCards_[i]) {
                focusedCardIdx = static_cast<int>(i);
                break;
            }
        }
    }
    if (focusWasInRight) {
        returnFocusToSidebar();
    }

    previewRoot_ = nullptr;
    openAllBtn_ = nullptr;
    previewCards_.clear();
    rightPanel->clearViews();

    CollectionPreviewData data;
    auto itCache = previewCache_.find(meta->id);
    if (itCache != previewCache_.end()) {
        data = itCache->second;
    }

    std::string countText;
    if (data.totalCount >= 0) {
        countText = util::pluralize("app/plural/games", data.totalCount);
    } else if (meta->isCollection) {
        countText = "app/collections/badge_collection"_i18n;
    }

    int maxMeta = 0;
    for (const auto& item : data.topGames) {
        if (item.has_metacritic) {
            int score = static_cast<int>(std::round(item.metacritic));
            if (score > maxMeta) maxMeta = score;
        }
    }

    NVGcolor genreColor = ThemeManager::instance().getGenreColor(meta->id);
    NVGcolor accentColor = sidebarIconColor(meta->id, genreColor);

    previewRoot_ = new brls::Box();
    previewRoot_->setAxis(brls::Axis::COLUMN);
    previewRoot_->setWidthPercentage(100.0f);
    previewRoot_->setGrow(1.0f);

    auto* banner = new CollectionPreviewBanner(
        meta->id,
        meta->title,
        meta->description,
        countText,
        maxMeta,
        genreColor,
        [this, id = meta->id]() { openSection(id); },
        [this]() { returnFocusToSidebar(); }
    );
    openAllBtn_ = banner->button();
    previewRoot_->addView(banner);

    // Section heading above the preview row
    brls::Box* headingRow = new brls::Box();
    headingRow->setFocusable(false);
    headingRow->setAxis(brls::Axis::ROW);
    headingRow->setAlignItems(brls::AlignItems::CENTER);
    headingRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    headingRow->setWidthPercentage(100.0f);
    headingRow->setHeight(28.0f);
    headingRow->setMarginBottom(10.0f);

    brls::Box* headingLeft = new brls::Box();
    headingLeft->setAxis(brls::Axis::ROW);
    headingLeft->setAlignItems(brls::AlignItems::CENTER);

    brls::Box* mark = new brls::Box();
    mark->setWidth(4.0f);
    mark->setHeight(16.0f);
    mark->setCornerRadius(2.0f);
    mark->setBackgroundColor(accentColor);
    mark->setMarginRight(8.0f);
    headingLeft->addView(mark);

    brls::Label* headingLbl = new brls::Label();
    headingLbl->setFontSize(style::kFontSub);
    headingLbl->setLetterSpacing(0.6f);
    headingLbl->setSingleLine(true);
    headingLbl->setIsWrapping(false);
    headingLbl->setMaxLines(1);
    headingLbl->setAutoAnimate(false);
    headingLbl->setTextColor(style::textPrimary());
    headingLbl->setText("app/collections/preview_heading"_i18n);
    headingLeft->addView(headingLbl);

    headingRow->addView(headingLeft);

    previewRoot_->addView(headingRow);

    if (!data.loaded && data.topGames.empty()) {
        brls::Box* loadingBox = new brls::Box();
        loadingBox->setGrow(1.0f);
        loadingBox->setAlignItems(brls::AlignItems::CENTER);
        loadingBox->setJustifyContent(brls::JustifyContent::CENTER);

        brls::Label* loadingLbl = new brls::Label();
        loadingLbl->setFontSize(style::kFontBody);
        loadingLbl->setTextColor(style::textSecondary());
        loadingLbl->setText("app/common/loading"_i18n);
        loadingBox->addView(loadingLbl);
        previewRoot_->addView(loadingBox);
    } else if (data.failed && data.topGames.empty()) {
        auto* empty = new EmptyState();
        empty->setContent(DashboardIcon::Kind::CloudError,
                          "app/empty/load_failed_title"_i18n,
                          "app/empty/load_failed_sub"_i18n);
        empty->setFocusable(false);
        previewRoot_->addView(empty);
    } else if (data.topGames.empty()) {
        auto* empty = new EmptyState();
        if (meta->id == "favorites") {
            empty->setContent(DashboardIcon::Kind::Star,
                              "app/empty/favorites_title"_i18n,
                              brls::getStr("app/empty/favorites_sub",
                                           brls::Hint::getKeyIcon(brls::ControllerButton::BUTTON_Y)));
        } else {
            empty->setContent(DashboardIcon::Kind::GameCards,
                              "app/empty/collection_title"_i18n,
                              "app/empty/collection_sub"_i18n);
        }
        empty->setFocusable(false);
        previewRoot_->addView(empty);
    } else {
        brls::Box* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::FLEX_START);
        row->setAlignItems(brls::AlignItems::FLEX_START);
        row->setWidthPercentage(100.0f);
        row->setHeight(308.0f);

        for (size_t i = 0; i < data.topGames.size(); ++i) {
            auto* card = new PreviewGameCard(
                data.topGames[i],
                static_cast<int>(i),
                previewImageToken_,
                [this]() { refreshFavoritesPreview(selectedId_ == "favorites"); },
                [this]() { returnFocusToSidebar(); }
            );
            if (i + 1 < kPreviewMaxGames) {
                card->setMarginRight(14.0f);
            }
            previewCards_.push_back(card);
            row->addView(card);
        }
        previewRoot_->addView(row);
    }

    rightPanel->addView(previewRoot_);
    updateNavigationRoutes();

    if (focusWasInRight) {
        if (focusWasOnOpenBtn && openAllBtn_) {
            brls::Application::giveFocus(openAllBtn_);
        } else if (!previewCards_.empty()) {
            int idx = (focusedCardIdx >= 0)
                ? std::min(focusedCardIdx, static_cast<int>(previewCards_.size()) - 1)
                : 0;
            brls::Application::giveFocus(previewCards_[static_cast<size_t>(idx)]);
        } else if (openAllBtn_) {
            brls::Application::giveFocus(openAllBtn_);
        }
    }

    if (animateFade && previewRoot_) {
        previewRoot_->alpha.stop();
        previewRoot_->alpha.reset(0.0f);
        previewRoot_->alpha.addStep(1.0f, 160, brls::EasingFunction::quadraticOut);
        previewRoot_->alpha.start();
    }
}

void CollectionsView::updateNavigationRoutes() {
    brls::View* activeRow = nullptr;
    auto itActive = sidebarRows_.find(selectedId_);
    if (itActive != sidebarRows_.end()) activeRow = itActive->second;
    if (!activeRow && !sidebarRowOrder_.empty()) activeRow = sidebarRowOrder_[0];

    brls::View* rightTarget = !previewCards_.empty()
        ? static_cast<brls::View*>(previewCards_[0])
        : static_cast<brls::View*>(openAllBtn_);

    for (size_t i = 0; i < sidebarRowOrder_.size(); ++i) {
        CollectionSidebarRow* row = sidebarRowOrder_[i];
        if (!row) continue;
        row->setCustomNavigationRoute(brls::FocusDirection::UP,
            (i > 0) ? static_cast<brls::View*>(sidebarRowOrder_[i - 1]) : nullptr);
        row->setCustomNavigationRoute(brls::FocusDirection::DOWN,
            (i + 1 < sidebarRowOrder_.size()) ? static_cast<brls::View*>(sidebarRowOrder_[i + 1]) : nullptr);
        row->setCustomNavigationRoute(brls::FocusDirection::LEFT, nullptr);
        row->setCustomNavigationRoute(brls::FocusDirection::RIGHT, rightTarget);
    }

    if (openAllBtn_) {
        openAllBtn_->setCustomNavigationRoute(brls::FocusDirection::LEFT, activeRow);
        openAllBtn_->setCustomNavigationRoute(brls::FocusDirection::DOWN,
            !previewCards_.empty() ? static_cast<brls::View*>(previewCards_[0]) : nullptr);
        openAllBtn_->setCustomNavigationRoute(brls::FocusDirection::UP, nullptr);
        openAllBtn_->setCustomNavigationRoute(brls::FocusDirection::RIGHT, nullptr);
    }

    for (size_t i = 0; i < previewCards_.size(); ++i) {
        brls::Box* card = previewCards_[i];
        if (!card) continue;
        card->setCustomNavigationRoute(brls::FocusDirection::UP, openAllBtn_);
        card->setCustomNavigationRoute(brls::FocusDirection::DOWN, nullptr);
        card->setCustomNavigationRoute(brls::FocusDirection::LEFT,
            (i > 0) ? static_cast<brls::View*>(previewCards_[i - 1]) : activeRow);
        card->setCustomNavigationRoute(brls::FocusDirection::RIGHT,
            (i + 1 < previewCards_.size()) ? static_cast<brls::View*>(previewCards_[i + 1]) : nullptr);
    }
}

void CollectionsView::openSection(const std::string& id) {
    if (id == "all_catalog") {
        brls::Application::pushActivity(new CatalogView());
        return;
    }
    if (id == "favorites") {
        brls::Application::pushActivity(new FavoritesView());
        return;
    }
    if (const SidebarEntryMeta* meta = findEntry(id); meta && meta->isCollection) {
        brls::Application::pushActivity(new CollectionGamesView(meta->collectionInfo));
    }
}

void CollectionsView::refreshFavoritesPreview(bool rebuildIfSelected) {
    auto snap = getCatalogSnapshot();
    CollectionPreviewData favData = buildFavoritesPreviewData(snap);
    previewCache_["favorites"] = favData;

    auto itRow = sidebarRows_.find("favorites");
    if (itRow != sidebarRows_.end() && itRow->second) {
        itRow->second->setCountText(std::to_string(favData.totalCount));
    }
    if (rebuildIfSelected) {
        rebuildPreviewPanel(false);
    }
}

void CollectionsView::openQuickSearch() {
    auto* ime = brls::Application::getImeManager();
    if (!ime) return;
    auto flag = alive_flag_;
    ime->openForText([flag](std::string query) {
        if (!flag->load()) return;
        std::string trimmed = trimCopy(query);
        if (trimmed.empty()) return;
        brls::Application::pushActivity(new CatalogView(trimmed));
    }, "app/actions/search"_i18n, "app/catalog/search_hint"_i18n, 64, "");
}

void CollectionsView::startBackgroundPreload() {
    std::shared_ptr<std::atomic<bool>> flag = alive_flag_;
    std::vector<catalog::CollectionInfo> collectionsCopy = catalog::CollectionsManager::instance().collections();

    brls::async([this, flag, collectionsCopy]() {
        if (g_appExiting.load() || !flag->load()) return;

        auto& statsMgr = catalog::GameStatsManager::instance();
        statsMgr.ensureLoaded();

        if (g_appExiting.load() || !flag->load()) return;
        auto catalogSnap = getCatalogSnapshot();

        // 1. Compute "all_catalog" and "ports_homebrew" immediately (in-memory, zero network)
        if (catalogSnap) {
            std::vector<PopularCandidate> officialCands;
            std::vector<PopularCandidate> homebrewCands;
            officialCands.reserve(catalogSnap->size());
            int officialTotal = 0;
            int homebrewTotal = 0;

            for (size_t i = 0; i < catalogSnap->size(); ++i) {
                if (!flag->load() || g_appExiting.load()) return;
                const Game& g = (*catalogSnap)[i];
                bool hb = isHomebrewGame(g);
                if (hb) ++homebrewTotal;
                else ++officialTotal;

                if (g.cover.empty()) continue;

                PopularCandidate cand;
                cand.game = &g;
                cand.origOrder = i;
                catalog::GameStats st;
                if (statsMgr.copySwitchStats(g.topic_id, st)) {
                    cand.hasStats = (st.downloads > 0 || st.seeds > 0);
                    cand.downloads = st.downloads;
                    cand.seeds = st.seeds;
                }
                if (hb) homebrewCands.push_back(cand);
                else officialCands.push_back(cand);
            }

            CollectionPreviewData allData;
            allData.loaded = true;
            allData.totalCount = officialTotal;
            allData.topGames = pickTopPopular(officialCands);

            CollectionPreviewData hbData;
            hbData.loaded = true;
            hbData.totalCount = homebrewTotal;
            hbData.topGames = pickTopPopular(homebrewCands);

            brls::sync([this, flag, allData = std::move(allData), hbData = std::move(hbData)]() mutable {
                if (!flag->load()) return;
                auto publish = [&](const std::string& id, CollectionPreviewData&& d) {
                    int cnt = d.totalCount;
                    previewCache_[id] = std::move(d);
                    auto itRow = sidebarRows_.find(id);
                    if (itRow != sidebarRows_.end() && itRow->second && cnt >= 0) {
                        itRow->second->setCountText(std::to_string(cnt));
                    }
                    if (selectedId_ == id) {
                        rebuildPreviewPanel(true);
                    }
                };
                publish("all_catalog", std::move(allData));
                publish("ports_homebrew", std::move(hbData));
            });
        }

        if (g_appExiting.load() || !flag->load()) return;

        // 2. Build shared match index ONCE for all collections
        catalog::CollectionMatchIndex matchIndex;
        if (catalogSnap) {
            matchIndex = catalog::buildMatchIndex(*catalogSnap);
        }

        // 3. Order remaining collections: cached-on-disk first, then uncached
        std::vector<catalog::CollectionInfo> cachedList;
        std::vector<catalog::CollectionInfo> networkList;
        for (const auto& info : collectionsCopy) {
            if (info.id == "ports_homebrew") continue;
            if (catalog::CollectionsManager::isCacheFresh(catalog::CollectionsManager::cachePathFor(info.id),
                                                          info.id == "new_release" ? (4 * 3600) : 86400)) {
                cachedList.push_back(info);
            } else {
                networkList.push_back(info);
            }
        }
        std::vector<catalog::CollectionInfo> pending;
        pending.reserve(cachedList.size() + networkList.size());
        pending.insert(pending.end(), cachedList.begin(), cachedList.end());
        pending.insert(pending.end(), networkList.begin(), networkList.end());

        while (!pending.empty()) {
            if (g_appExiting.load() || !flag->load()) return;

            std::string prioId;
            {
                std::lock_guard<std::mutex> lock(priorityMutex_);
                prioId = priorityCollectionId_;
            }

            size_t pickIdx = 0;
            if (!prioId.empty()) {
                for (size_t i = 0; i < pending.size(); ++i) {
                    if (pending[i].id == prioId) {
                        pickIdx = i;
                        break;
                    }
                }
            }
            catalog::CollectionInfo info = std::move(pending[pickIdx]);
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(pickIdx));

            std::vector<catalog::CollectionEntry> entries;
            bool fromCache = true;
            bool ok = catalog::CollectionsManager::instance().loadCollection(info, entries, fromCache);
            if (g_appExiting.load() || !flag->load()) return;

            CollectionPreviewData preview;
            preview.loaded = true;
            preview.failed = !ok;

            if (ok) {
                preview.totalCount = static_cast<int>(entries.size());

                if (info.id == "new_release" || info.id == "top_100") {
                    DedupeTracker dedupe;
                    for (const auto& entry : entries) {
                        if (!flag->load() || g_appExiting.load()) return;
                        const Game* matched = catalog::matchWithIndex(matchIndex, entry);
                        if (!matched || !dedupe.accept(*matched, true)) continue;
                        PreviewGameItem item;
                        item.game = *matched;
                        item.metacritic = entry.metacritic;
                        item.has_metacritic = entry.has_metacritic;
                        preview.topGames.push_back(std::move(item));
                        if (preview.topGames.size() >= kPreviewMaxGames) break;
                    }
                } else {
                    std::vector<PopularCandidate> candidates;
                    candidates.reserve(entries.size());
                    for (size_t i = 0; i < entries.size(); ++i) {
                        if (!flag->load() || g_appExiting.load()) return;
                        const Game* matched = catalog::matchWithIndex(matchIndex, entries[i]);
                        if (!matched || matched->cover.empty()) continue;

                        PopularCandidate cand;
                        cand.game = matched;
                        cand.metacritic = entries[i].metacritic;
                        cand.has_metacritic = entries[i].has_metacritic;
                        cand.origOrder = i;

                        catalog::GameStats st;
                        if (statsMgr.copySwitchStats(matched->topic_id, st)) {
                            cand.hasStats = (st.downloads > 0 || st.seeds > 0);
                            cand.downloads = st.downloads;
                            cand.seeds = st.seeds;
                        }
                        candidates.push_back(cand);
                    }
                    preview.topGames = pickTopPopular(candidates);
                }
            }

            std::string cid = info.id;
            brls::sync([this, flag, cid, preview = std::move(preview)]() mutable {
                if (!flag->load()) return;
                int cnt = preview.totalCount;
                previewCache_[cid] = std::move(preview);

                auto itRow = sidebarRows_.find(cid);
                if (itRow != sidebarRows_.end() && itRow->second && cnt >= 0) {
                    itRow->second->setCountText(std::to_string(cnt));
                }
                if (selectedId_ == cid) {
                    rebuildPreviewPanel(true);
                }
            });
        }
    });
}

} // namespace ui
