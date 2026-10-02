#include "ThemeManager.hpp"
#include "../config/config.h"
#include "../utils/log.h"
#include <filesystem>
#include <algorithm>

namespace ui {

ThemeManager& ThemeManager::instance() {
    static ThemeManager inst;
    return inst;
}

ThemeManager::ThemeManager() {
    themes_ = {
        {
            "emerald",
            "app/theme/emerald",
            nvgRGB(0, 224, 165),
            nvgRGB(16, 231, 176),
            nvgRGBA(0, 224, 165, 38),
            nvgRGBA(0, 224, 165, 28),
            nvgRGBA(0, 224, 165, 80),
            nvgRGBA(0, 224, 165, 85),
            nvgRGBA(0, 224, 165, 0),
            nvgRGBA(8, 22, 18, 140),
            nvgRGBA(0, 224, 165, 50),
            nvgRGBA(245, 252, 248, 245),
            nvgRGBA(160, 210, 195, 235),
            "img/backgrounds/bg_emerald.jpg",
            false,
            nvgRGBA(250, 252, 255, 255),
            nvgRGBA(180, 210, 200, 230),
            nvgRGB(16, 235, 175)
        },
        {
            "cyberpunk",
            "app/theme/cyberpunk",
            nvgRGB(0, 210, 255),
            nvgRGB(50, 230, 255),
            nvgRGBA(0, 210, 255, 38),
            nvgRGBA(0, 210, 255, 28),
            nvgRGBA(0, 210, 255, 80),
            nvgRGBA(0, 210, 255, 85),
            nvgRGBA(0, 210, 255, 0),
            nvgRGBA(8, 18, 30, 140),
            nvgRGBA(0, 210, 255, 50),
            nvgRGBA(245, 252, 255, 245),
            nvgRGBA(160, 205, 235, 235),
            "img/backgrounds/bg_cyberpunk.jpg",
            false,
            nvgRGBA(250, 252, 255, 255),
            nvgRGBA(180, 205, 230, 230),
            nvgRGB(50, 225, 255)
        },
        {
            "ruby",
            "app/theme/ruby",
            nvgRGB(255, 75, 75),
            nvgRGB(255, 120, 120),
            nvgRGBA(255, 75, 75, 38),
            nvgRGBA(255, 75, 75, 28),
            nvgRGBA(255, 75, 75, 80),
            nvgRGBA(255, 75, 75, 85),
            nvgRGBA(255, 75, 75, 0),
            nvgRGBA(24, 10, 14, 140),
            nvgRGBA(255, 75, 75, 50),
            nvgRGBA(255, 245, 245, 245),
            nvgRGBA(240, 195, 195, 235),
            "img/backgrounds/bg_ruby.jpg",
            false,
            nvgRGBA(250, 252, 255, 255),
            nvgRGBA(230, 200, 205, 230),
            nvgRGB(255, 115, 115)
        },
        {
            "amethyst",
            "app/theme/amethyst",
            nvgRGB(190, 95, 255),
            nvgRGB(215, 135, 255),
            nvgRGBA(190, 95, 255, 38),
            nvgRGBA(190, 95, 255, 28),
            nvgRGBA(190, 95, 255, 80),
            nvgRGBA(190, 95, 255, 85),
            nvgRGBA(190, 95, 255, 0),
            nvgRGBA(20, 10, 28, 140),
            nvgRGBA(190, 95, 255, 50),
            nvgRGBA(252, 245, 255, 245),
            nvgRGBA(225, 195, 245, 235),
            "img/backgrounds/bg_amethyst.jpg",
            false,
            nvgRGBA(250, 252, 255, 255),
            nvgRGBA(220, 200, 235, 230),
            nvgRGB(220, 140, 255)
        },
        {
            "amber",
            "app/theme/amber",
            nvgRGB(255, 165, 30),
            nvgRGB(255, 195, 70),
            nvgRGBA(255, 165, 30, 38),
            nvgRGBA(255, 165, 30, 28),
            nvgRGBA(255, 165, 30, 80),
            nvgRGBA(255, 165, 30, 85),
            nvgRGBA(255, 165, 30, 0),
            nvgRGBA(24, 14, 8, 140),
            nvgRGBA(255, 165, 30, 50),
            nvgRGBA(255, 250, 240, 245),
            nvgRGBA(240, 210, 170, 235),
            "img/backgrounds/bg_amber.jpg",
            false,
            nvgRGBA(250, 252, 255, 255),
            nvgRGBA(230, 210, 190, 230),
            nvgRGB(255, 195, 60)
        },
        {
            "light",
            "app/theme/light",
            nvgRGB(0, 155, 230),       // Nintendo Switch Blue #009BE6
            nvgRGB(0, 200, 245),       // Nintendo Switch Cyan #00C8F5
            nvgRGBA(0, 155, 230, 50),  // glow
            nvgRGBA(0, 155, 230, 25),  // dim
            nvgRGBA(0, 155, 230, 65),  // medium
            nvgRGBA(0, 155, 230, 120), // sparkline top
            nvgRGBA(0, 155, 230, 10),  // sparkline bot
            nvgRGBA(255, 255, 255, 245), // cardBg (crisp white card)
            nvgRGBA(210, 218, 228, 190), // cardBorder
            nvgRGBA(36, 42, 54, 255),    // cardTitle (dark charcoal #242A36)
            nvgRGBA(100, 112, 128, 240), // cardSub (slate grey)
            "img/backgrounds/bg_light.jpg",
            true,
            nvgRGBA(36, 42, 54, 255),    // textPrimary
            nvgRGBA(100, 112, 128, 240), // textSecondary
            nvgRGB(0, 140, 225)          // textAccent
        },
        {
            "graphite",
            "app/theme/graphite",
            nvgRGB(195, 205, 220),
            nvgRGB(230, 238, 248),
            nvgRGBA(195, 205, 220, 40),
            nvgRGBA(195, 205, 220, 26),
            nvgRGBA(195, 205, 220, 75),
            nvgRGBA(230, 238, 248, 85),
            nvgRGBA(195, 205, 220, 0),
            nvgRGBA(14, 17, 22, 160),
            nvgRGBA(195, 205, 220, 45),
            nvgRGBA(250, 252, 255, 245),
            nvgRGBA(180, 195, 215, 235),
            "img/backgrounds/bg_graphite.jpg",
            false,
            nvgRGBA(250, 252, 255, 255),
            nvgRGBA(185, 200, 218, 230),
            nvgRGB(220, 230, 245)
        }
    };

    for (size_t i = 0; i < themes_.size(); ++i) {
        if (themes_[i].id == "light") {
            currentThemeIdx_ = i;
            break;
        }
    }
}

void ThemeManager::init() {
    auto& cfg = config::ConfigManager::instance();
    std::string themeId = cfg.getTheme();
    for (size_t i = 0; i < themes_.size(); ++i) {
        if (themes_[i].id == themeId) {
            currentThemeIdx_ = i;
            break;
        }
    }
    applyThemeToBorealis();
    brls::Application::setGlobalWallpaperBlur(blurLevelToRadius(cfg.getBackgroundBlur()));
    brls::Application::setGlobalWallpaperMainMenuDim(dimLevelToAlpha(cfg.getBackgroundDim()));
    util::logLine("ThemeManager: initialized with theme=" + getCurrentThemeId() + ", wallpaper=" + getEffectiveWallpaperPath());
}

const std::string& ThemeManager::getCurrentThemeId() const {
    return themes_[currentThemeIdx_].id;
}

const ThemePalette& ThemeManager::getCurrentTheme() const {
    return themes_[currentThemeIdx_];
}

const std::vector<ThemePalette>& ThemeManager::getAllThemes() const {
    return themes_;
}

void ThemeManager::setTheme(const std::string& themeId) {
    for (size_t i = 0; i < themes_.size(); ++i) {
        if (themes_[i].id == themeId) {
            currentThemeIdx_ = i;
            break;
        }
    }
    auto& cfg = config::ConfigManager::instance();
    cfg.setTheme(getCurrentThemeId());
    applyThemeToBorealis();

    // If background mode is auto, notify wallpaper change as theme changed default wallpaper
    if (cfg.getBackgroundMode() == "auto") {
        notifyWallpaperChanged();
    }

    for (const auto& cb : listeners_) {
        if (cb) cb();
    }
    util::logLine("ThemeManager: theme set to " + getCurrentThemeId());
}

bool ThemeManager::isCurrentThemeLight() const {
    return themes_[currentThemeIdx_].isLight;
}

NVGcolor ThemeManager::getTextPrimaryColor() const {
    return themes_[currentThemeIdx_].textPrimary;
}

NVGcolor ThemeManager::getTextSecondaryColor() const {
    return themes_[currentThemeIdx_].textSecondary;
}

NVGcolor ThemeManager::getTextAccentColor() const {
    return themes_[currentThemeIdx_].textAccent;
}

NVGcolor ThemeManager::getAccentColor() const {
    return themes_[currentThemeIdx_].primary;
}

NVGcolor ThemeManager::getSecondaryColor() const {
    return themes_[currentThemeIdx_].secondary;
}

NVGcolor ThemeManager::getAccentDimColor() const {
    return themes_[currentThemeIdx_].accentDim;
}

NVGcolor ThemeManager::getAccentMediumColor() const {
    return themes_[currentThemeIdx_].accentMedium;
}

NVGcolor ThemeManager::getSparklineTopColor() const {
    return themes_[currentThemeIdx_].sparklineTop;
}

NVGcolor ThemeManager::getSparklineBottomColor() const {
    return themes_[currentThemeIdx_].sparklineBottom;
}

NVGcolor ThemeManager::getCardBgColor() const {
    return themes_[currentThemeIdx_].cardBg;
}

NVGcolor ThemeManager::getCardBorderColor() const {
    return themes_[currentThemeIdx_].cardBorder;
}

NVGcolor ThemeManager::getCardTitleColor() const {
    return themes_[currentThemeIdx_].cardTitle;
}

NVGcolor ThemeManager::getCardSubColor() const {
    return themes_[currentThemeIdx_].cardSub;
}

NVGcolor ThemeManager::getGenreColor(const std::string& genreId) const {
    if (genreId == "all_catalog") return getAccentColor();
    if (genreId == "favorites") return nvgRGBA(255, 205, 30, 255);
    if (genreId == "top_100") return nvgRGBA(255, 215, 0, 255);
    if (genreId == "new_release") return getSecondaryColor();

    const std::string& th = getCurrentThemeId();
    if (th == "amber") {
        if (genreId == "action_adventure")    return nvgRGBA(255, 110, 30, 255);
        if (genreId == "arcade")              return nvgRGBA(230, 160, 40, 255);
        if (genreId == "horror")              return nvgRGBA(210, 85, 45, 255);
        if (genreId == "metroidvania")        return nvgRGBA(255, 175, 50, 255);
        if (genreId == "party_multiplayer")   return nvgRGBA(240, 195, 60, 255);
        if (genreId == "platformers")         return nvgRGBA(255, 140, 25, 255);
        if (genreId == "puzzles")             return nvgRGBA(225, 170, 75, 255);
        if (genreId == "roguelike_roguelite") return nvgRGBA(190, 130, 80, 255);
        if (genreId == "rpg_jrpg")            return nvgRGBA(255, 130, 70, 255);
        if (genreId == "shooters")            return nvgRGBA(245, 90, 35, 255);
        if (genreId == "simulation_cozy")     return nvgRGBA(220, 185, 90, 255);
        if (genreId == "strategy_tactics")    return nvgRGBA(180, 145, 110, 255);
        if (genreId == "visual_novels")       return nvgRGBA(235, 150, 95, 255);
        if (genreId == "ports_homebrew")      return nvgRGBA(255, 165, 40, 255);
    } else if (th == "cyberpunk") {
        if (genreId == "action_adventure")    return nvgRGBA(255, 50, 150, 255);
        if (genreId == "arcade")              return nvgRGBA(180, 70, 255, 255);
        if (genreId == "horror")              return nvgRGBA(255, 40, 90, 255);
        if (genreId == "metroidvania")        return nvgRGBA(0, 220, 255, 255);
        if (genreId == "party_multiplayer")   return nvgRGBA(0, 245, 180, 255);
        if (genreId == "platformers")         return nvgRGBA(0, 190, 255, 255);
        if (genreId == "puzzles")             return nvgRGBA(130, 90, 255, 255);
        if (genreId == "roguelike_roguelite") return nvgRGBA(170, 140, 220, 255);
        if (genreId == "rpg_jrpg")            return nvgRGBA(255, 80, 200, 255);
        if (genreId == "shooters")            return nvgRGBA(255, 60, 100, 255);
        if (genreId == "simulation_cozy")     return nvgRGBA(0, 230, 210, 255);
        if (genreId == "strategy_tactics")    return nvgRGBA(90, 160, 230, 255);
        if (genreId == "visual_novels")       return nvgRGBA(220, 100, 255, 255);
        if (genreId == "ports_homebrew")      return nvgRGBA(0, 210, 255, 255);
    } else if (th == "ruby") {
        if (genreId == "action_adventure")    return nvgRGBA(255, 75, 75, 255);
        if (genreId == "arcade")              return nvgRGBA(255, 115, 115, 255);
        if (genreId == "horror")              return nvgRGBA(220, 45, 45, 255);
        if (genreId == "metroidvania")        return nvgRGBA(255, 90, 90, 255);
        if (genreId == "party_multiplayer")   return nvgRGBA(255, 135, 105, 255);
        if (genreId == "platformers")         return nvgRGBA(255, 60, 60, 255);
        if (genreId == "puzzles")             return nvgRGBA(235, 105, 105, 255);
        if (genreId == "roguelike_roguelite") return nvgRGBA(195, 95, 95, 255);
        if (genreId == "rpg_jrpg")            return nvgRGBA(255, 80, 120, 255);
        if (genreId == "shooters")            return nvgRGBA(240, 50, 50, 255);
        if (genreId == "simulation_cozy")     return nvgRGBA(255, 145, 130, 255);
        if (genreId == "strategy_tactics")    return nvgRGBA(180, 110, 110, 255);
        if (genreId == "visual_novels")       return nvgRGBA(245, 110, 140, 255);
        if (genreId == "ports_homebrew")      return nvgRGBA(255, 70, 70, 255);
    } else if (th == "amethyst") {
        if (genreId == "action_adventure")    return nvgRGBA(195, 95, 255, 255);
        if (genreId == "arcade")              return nvgRGBA(215, 120, 255, 255);
        if (genreId == "horror")              return nvgRGBA(165, 70, 230, 255);
        if (genreId == "metroidvania")        return nvgRGBA(180, 85, 245, 255);
        if (genreId == "party_multiplayer")   return nvgRGBA(210, 140, 255, 255);
        if (genreId == "platformers")         return nvgRGBA(170, 75, 240, 255);
        if (genreId == "puzzles")             return nvgRGBA(190, 110, 250, 255);
        if (genreId == "roguelike_roguelite") return nvgRGBA(150, 105, 195, 255);
        if (genreId == "rpg_jrpg")            return nvgRGBA(225, 90, 235, 255);
        if (genreId == "shooters")            return nvgRGBA(185, 65, 220, 255);
        if (genreId == "simulation_cozy")     return nvgRGBA(205, 150, 255, 255);
        if (genreId == "strategy_tactics")    return nvgRGBA(140, 115, 180, 255);
        if (genreId == "visual_novels")       return nvgRGBA(220, 125, 245, 255);
    } else if (th == "light") {
        if (genreId == "action_adventure")    return nvgRGBA(0, 140, 220, 255);
        if (genreId == "arcade")              return nvgRGBA(0, 165, 230, 255);
        if (genreId == "horror")              return nvgRGBA(140, 70, 180, 255);
        if (genreId == "metroidvania")        return nvgRGBA(0, 175, 210, 255);
        if (genreId == "party_multiplayer")   return nvgRGBA(230, 110, 30, 255);
        if (genreId == "platformers")         return nvgRGBA(0, 150, 220, 255);
        if (genreId == "puzzles")             return nvgRGBA(150, 100, 210, 255);
        if (genreId == "roguelike_roguelite") return nvgRGBA(180, 90, 70, 255);
        if (genreId == "rpg_jrpg")            return nvgRGBA(190, 60, 120, 255);
        if (genreId == "shooters")            return nvgRGBA(220, 70, 60, 255);
        if (genreId == "simulation_cozy")     return nvgRGBA(30, 170, 140, 255);
        if (genreId == "strategy_tactics")    return nvgRGBA(70, 120, 190, 255);
        if (genreId == "visual_novels")       return nvgRGBA(200, 70, 150, 255);
        if (genreId == "ports_homebrew")      return nvgRGBA(0, 160, 220, 255);
    } else if (th == "graphite") {
        if (genreId == "action_adventure")    return nvgRGBA(190, 200, 215, 255);
        if (genreId == "arcade")              return nvgRGBA(205, 215, 230, 255);
        if (genreId == "horror")              return nvgRGBA(140, 150, 165, 255);
        if (genreId == "metroidvania")        return nvgRGBA(185, 195, 210, 255);
        if (genreId == "party_multiplayer")   return nvgRGBA(210, 220, 235, 255);
        if (genreId == "platformers")         return nvgRGBA(195, 205, 220, 255);
        if (genreId == "puzzles")             return nvgRGBA(180, 190, 205, 255);
        if (genreId == "roguelike_roguelite") return nvgRGBA(160, 170, 185, 255);
        if (genreId == "rpg_jrpg")            return nvgRGBA(215, 225, 240, 255);
        if (genreId == "shooters")            return nvgRGBA(175, 185, 200, 255);
        if (genreId == "simulation_cozy")     return nvgRGBA(195, 205, 218, 255);
        if (genreId == "strategy_tactics")    return nvgRGBA(165, 175, 190, 255);
        if (genreId == "visual_novels")       return nvgRGBA(200, 210, 225, 255);
        if (genreId == "ports_homebrew")      return nvgRGBA(190, 200, 215, 255);
    } else {
        // Emerald default
        if (genreId == "action_adventure")    return nvgRGBA(0, 220, 140, 255);
        if (genreId == "arcade")              return nvgRGBA(50, 230, 170, 255);
        if (genreId == "horror")              return nvgRGBA(20, 190, 130, 255);
        if (genreId == "metroidvania")        return nvgRGBA(0, 230, 190, 255);
        if (genreId == "party_multiplayer")   return nvgRGBA(80, 235, 150, 255);
        if (genreId == "platformers")         return nvgRGBA(0, 215, 165, 255);
        if (genreId == "puzzles")             return nvgRGBA(40, 220, 180, 255);
        if (genreId == "roguelike_roguelite") return nvgRGBA(60, 180, 140, 255);
        if (genreId == "rpg_jrpg")            return nvgRGBA(0, 240, 160, 255);
        if (genreId == "shooters")            return nvgRGBA(10, 210, 130, 255);
        if (genreId == "simulation_cozy")     return nvgRGBA(100, 230, 130, 255);
        if (genreId == "strategy_tactics")    return nvgRGBA(80, 175, 155, 255);
        if (genreId == "visual_novels")       return nvgRGBA(30, 225, 190, 255);
        if (genreId == "ports_homebrew")      return nvgRGBA(0, 210, 180, 255);
    }
    return getAccentColor();
}

std::string ThemeManager::getBackgroundMode() const {
    return config::ConfigManager::instance().getBackgroundMode();
}

void ThemeManager::setBackgroundMode(const std::string& mode) {
    auto& cfg = config::ConfigManager::instance();
    cfg.setBackgroundMode(mode);
    notifyWallpaperChanged();
}

std::string ThemeManager::getCustomBackgroundPath() const {
    return config::ConfigManager::instance().getCustomBackgroundPath();
}

void ThemeManager::setCustomBackgroundPath(const std::string& path) {
    auto& cfg = config::ConfigManager::instance();
    cfg.setCustomBackgroundPath(path);
    cfg.setBackgroundMode("custom");
    notifyWallpaperChanged();
}

int ThemeManager::getBackgroundBlurLevel() const {
    return config::ConfigManager::instance().getBackgroundBlur();
}

void ThemeManager::setBackgroundBlurLevel(int level) {
    auto& cfg = config::ConfigManager::instance();
    cfg.setBackgroundBlur(level);
    brls::Application::setGlobalWallpaperBlur(blurLevelToRadius(level));
    notifyWallpaperChanged();
}

int ThemeManager::blurLevelToRadius(int level) {
    switch (level) {
        case 1: return 8;   // Low
        case 2: return 18;  // Medium
        case 3: return 32;  // High
        default: return 0;  // Off
    }
}

int ThemeManager::getBackgroundDimLevel() const {
    return config::ConfigManager::instance().getBackgroundDim();
}

void ThemeManager::setBackgroundDimLevel(int level) {
    auto& cfg = config::ConfigManager::instance();
    cfg.setBackgroundDim(level);
    brls::Application::setGlobalWallpaperMainMenuDim(dimLevelToAlpha(level));
    notifyWallpaperChanged();
}

float ThemeManager::dimLevelToAlpha(int level) {
    switch (level) {
        case 0: return 0.0f;  // 0%
        case 1: return 0.15f; // 15%
        case 2: return 0.28f; // 28% Standard
        case 3: return 0.45f; // 45%
        case 4: return 0.65f; // 65%
        default: return 0.28f;
    }
}

std::string ThemeManager::getEffectiveWallpaperPath() const {
    auto& cfg = config::ConfigManager::instance();
    std::string mode = cfg.getBackgroundMode();

    if (mode == "custom") {
        std::string customPath = cfg.getCustomBackgroundPath();
        if (!customPath.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(customPath, ec)) {
                return customPath;
            }
        }
    }

    std::string relFile;
    if (mode == "auto" || mode.empty()) {
        relFile = getCurrentTheme().defaultWallpaperFile;
    } else {
        // Mode could be a specific theme id (emerald, cyberpunk, ruby, amethyst, amber)
        for (const auto& th : themes_) {
            if (th.id == mode) {
                relFile = th.defaultWallpaperFile;
                break;
            }
        }
        if (relFile.empty()) {
            relFile = getCurrentTheme().defaultWallpaperFile;
        }
    }

    std::string fullPath = std::string(BRLS_RESOURCES) + relFile;
    std::error_code ec;
    if (std::filesystem::exists(fullPath, ec)) {
        return fullPath;
    }

    // Fallback to legacy dashboard_bg.jpg if specific theme wallpaper not found
    std::string fallback = std::string(BRLS_RESOURCES) + "img/dashboard_bg.jpg";
    if (std::filesystem::exists(fallback, ec)) {
        return fallback;
    }

    return fullPath;
}

void ThemeManager::notifyWallpaperChanged() {
    wallpaperVersion_++;
    util::logLine("ThemeManager: wallpaper changed, version=" + std::to_string(wallpaperVersion_) + " path=" + getEffectiveWallpaperPath());
    for (const auto& cb : listeners_) {
        if (cb) cb();
    }
}

void ThemeManager::subscribe(ThemeChangeCallback cb) {
    listeners_.push_back(std::move(cb));
}

void ThemeManager::applyThemeToBorealis() {
    NVGcolor primary = getAccentColor();
    NVGcolor secondary = getSecondaryColor();
    NVGcolor pulse = themes_[currentThemeIdx_].primaryGlow;
    bool isLight = themes_[currentThemeIdx_].isLight;

    if (brls::Application::getPlatform()) {
        brls::Application::getPlatform()->setThemeVariant(isLight ? brls::ThemeVariant::LIGHT : brls::ThemeVariant::DARK);
    }

    brls::Theme& darkTheme = brls::Theme::getDarkTheme();
    darkTheme.addColor("brls/accent", primary);
    darkTheme.addColor("brls/highlight/color1", primary);
    darkTheme.addColor("brls/highlight/color2", secondary);
    darkTheme.addColor("brls/click_pulse", pulse);
    darkTheme.addColor("brls/sidebar/active_item", primary);
    darkTheme.addColor("brls/button/primary_enabled_background", primary);
    darkTheme.addColor("brls/list/listItem_value_color", primary);
    darkTheme.addColor("brls/slider/line_filled", primary);
    darkTheme.addColor("brls/text", nvgRGBA(250, 252, 255, 255));
    darkTheme.addColor("brls/text_disabled", nvgRGB(140, 150, 165));
    darkTheme.addColor("brls/header/subtitle", nvgRGBA(180, 195, 210, 230));
    darkTheme.addColor("brls/text_secondary", nvgRGBA(180, 195, 210, 230));

    brls::Theme& lightTheme = brls::Theme::getLightTheme();
    lightTheme.addColor("brls/accent", primary);
    lightTheme.addColor("brls/highlight/color1", primary);
    lightTheme.addColor("brls/highlight/color2", secondary);
    lightTheme.addColor("brls/click_pulse", pulse);
    lightTheme.addColor("brls/sidebar/active_item", primary);
    lightTheme.addColor("brls/button/primary_enabled_background", primary);
    lightTheme.addColor("brls/list/listItem_value_color", primary);
    lightTheme.addColor("brls/slider/line_filled", primary);
    lightTheme.addColor("brls/text", nvgRGB(36, 42, 54));
    lightTheme.addColor("brls/text_disabled", nvgRGB(130, 140, 155));
    lightTheme.addColor("brls/header/subtitle", nvgRGB(100, 112, 128));
    lightTheme.addColor("brls/text_secondary", nvgRGB(100, 112, 128));
    lightTheme.addColor("brls/sidebar/separator", nvgRGB(220, 226, 235));
    lightTheme.addColor("brls/applet_frame/separator", nvgRGB(215, 222, 232));
    lightTheme.addColor("brls/header/border", nvgRGB(215, 222, 232));
}

} // namespace ui
