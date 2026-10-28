#pragma once

// Shared look of the interface: type scale, spacing, corner radii and text /
// state colors. Sizes are in px at 1280x720, the Switch screen.

#include <borealis.hpp>
#include "../ThemeManager.hpp"

namespace ui::style {

// ---- Type scale ----------------------------------------------------------
// Nothing smaller than kFontCaption: 14 px is about 1.6 mm on the handheld screen.
constexpr float kFontScreenTitle = 28.0f; // screen header
constexpr float kFontValue       = 32.0f; // large number on a card
constexpr float kFontValueMid    = 24.0f; // large text value that may be long
constexpr float kFontItemTitle   = 18.0f; // card / list item title
constexpr float kFontBody        = 16.0f; // body text, descriptions
constexpr float kFontSub         = 15.0f; // sub line, metadata
constexpr float kFontCaption     = 14.0f; // caption, chip, header meta

// ---- Spacing (8 px grid) -------------------------------------------------
constexpr float kSpaceXs = 4.0f;
constexpr float kSpaceS  = 8.0f;
constexpr float kSpaceM  = 16.0f;
constexpr float kSpaceL  = 24.0f;
constexpr float kSpaceXl = 32.0f;

// ---- Corner radii --------------------------------------------------------
constexpr float kRadiusChip  = 6.0f;
constexpr float kRadiusCover = 8.0f;
constexpr float kRadiusCard  = 12.0f;
constexpr float kRadiusPanel = 16.0f;

// ---- Focus ---------------------------------------------------------------
constexpr float kFocusBorder = 3.0f;
constexpr float kFocusScale  = 1.03f;

// ---- Colors --------------------------------------------------------------
inline bool isLight() {
    return ThemeManager::instance().isCurrentThemeLight();
}

// Main text.
inline NVGcolor textPrimary() {
    return ThemeManager::instance().getTextPrimaryColor();
}

// Captions, sub lines, metadata. Darker than the theme's own grey in the light
// theme so 14-15 px text stays readable on white cards.
inline NVGcolor textSecondary() {
    return isLight() ? nvgRGB(90, 99, 114) : ThemeManager::instance().getTextSecondaryColor();
}

// Section headers, focused titles, selected values.
inline NVGcolor textAccent() {
    return ThemeManager::instance().getTextAccentColor();
}

inline NVGcolor cardBg()     { return ThemeManager::instance().getCardBgColor(); }
inline NVGcolor cardBorder() { return ThemeManager::instance().getCardBorderColor(); }

// States: green = all good, orange = needs an action, red = error.
inline NVGcolor stateOk()          { return isLight() ? nvgRGB(29, 122, 70)   : nvgRGB(110, 220, 150); }
inline NVGcolor stateWarn()        { return isLight() ? nvgRGB(179, 89, 0)    : nvgRGB(255, 185, 70); }
inline NVGcolor stateWarnBorder()  { return isLight() ? nvgRGB(243, 201, 154) : nvgRGBA(255, 185, 70, 120); }
inline NVGcolor stateError()       { return isLight() ? nvgRGB(196, 43, 28)   : nvgRGB(255, 105, 97); }

// Empty part of progress and storage bars.
inline NVGcolor barTrack() { return isLight() ? nvgRGB(226, 231, 238) : nvgRGBA(255, 255, 255, 34); }

} // namespace ui::style
