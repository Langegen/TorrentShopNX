#pragma once

#include <borealis.hpp>
#include <string>
#include <vector>
#include <functional>

namespace ui {

struct ThemePalette {
    std::string id;
    std::string nameKey;
    NVGcolor primary;         // Primary accent color (badges, text, icons, buttons)
    NVGcolor secondary;       // Secondary highlight color (gradients, accents)
    NVGcolor primaryGlow;     // Pulse & focus glow
    NVGcolor accentDim;       // 10-15% tint for badge/pill backgrounds
    NVGcolor accentMedium;    // 30-35% tint
    NVGcolor sparklineTop;    // Speed graph top fill
    NVGcolor sparklineBottom; // Speed graph bottom fill
    NVGcolor cardBg;          // Theme-tinted frosted glass for cards & info blocks
    NVGcolor cardBorder;      // Theme-tinted subtle border
    NVGcolor cardTitle;       // Primary text inside cards
    NVGcolor cardSub;         // Secondary / hint text inside cards
    std::string defaultWallpaperFile; // Relative to BRLS_RESOURCES
    bool isLight = false;     // True for light theme (drives light UI mode, dark text, light glass)
    NVGcolor textPrimary;     // High-contrast primary text (white on dark, dark slate on light)
    NVGcolor textSecondary;   // Subtle secondary text
    NVGcolor textAccent;      // High-contrast vibrant accent for headers/highlights
};

class ThemeManager {
public:
    static ThemeManager& instance();

    void init();

    // Theme preset operations
    const std::string& getCurrentThemeId() const;
    void setTheme(const std::string& themeId);
    const ThemePalette& getCurrentTheme() const;
    const std::vector<ThemePalette>& getAllThemes() const;

    // Theme properties & text contrast accessors
    bool isCurrentThemeLight() const;
    NVGcolor getTextPrimaryColor() const;
    NVGcolor getTextSecondaryColor() const;
    NVGcolor getTextAccentColor() const;

    // Direct color accessors
    NVGcolor getAccentColor() const;
    NVGcolor getSecondaryColor() const;
    NVGcolor getAccentDimColor() const;
    NVGcolor getAccentMediumColor() const;
    NVGcolor getDimAccentColor() const { return getAccentDimColor(); }
    NVGcolor getMediumAccentColor() const { return getAccentMediumColor(); }
    NVGcolor getSparklineTopColor() const;
    NVGcolor getSparklineBottomColor() const;
    NVGcolor getCardBgColor() const;
    NVGcolor getCardBorderColor() const;
    NVGcolor getCardTitleColor() const;
    NVGcolor getCardSubColor() const;
    NVGcolor getGenreColor(const std::string& genreId) const;

    // Background wallpaper operations
    // modes: "auto", "emerald", "cyberpunk", "ruby", "amethyst", "amber", "custom"
    std::string getBackgroundMode() const;
    void setBackgroundMode(const std::string& mode);
    std::string getCustomBackgroundPath() const;
    void setCustomBackgroundPath(const std::string& path);

    // Background blur & dimming operations
    int getBackgroundBlurLevel() const;
    void setBackgroundBlurLevel(int level);
    static int blurLevelToRadius(int level);

    int getBackgroundDimLevel() const;
    void setBackgroundDimLevel(int level);
    static float dimLevelToAlpha(int level);

    // Returns the absolute file path to the effective wallpaper image
    std::string getEffectiveWallpaperPath() const;

    // Wallpaper reload tracking
    uint32_t getWallpaperVersion() const { return wallpaperVersion_; }
    void notifyWallpaperChanged();

    // Listener callbacks
    using ThemeChangeCallback = std::function<void()>;
    void subscribe(ThemeChangeCallback cb);

private:
    ThemeManager();
    ~ThemeManager() = default;

    void applyThemeToBorealis();

    std::vector<ThemePalette> themes_;
    size_t currentThemeIdx_ = 0;
    uint32_t wallpaperVersion_ = 1;
    std::vector<ThemeChangeCallback> listeners_;
};

} // namespace ui
