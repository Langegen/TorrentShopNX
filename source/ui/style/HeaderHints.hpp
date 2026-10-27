#pragma once

// Button hints shown in a screen header ("Игр: 53   ·   [LS] Вид   [−] Обновить").
// Only for actions the footer does not list: the footer has room for about five
// hints on the Switch, next to the battery, Wi-Fi and clock.
//
// The button glyphs are the footer's own (brls::Hint::getKeyIcon), so they match
// it and follow the A/B swap setting.

#include <borealis.hpp>
#include <initializer_list>
#include <string>

namespace ui::style {

// "<icon> label"
inline std::string buttonHint(brls::ControllerButton button, const std::string& label) {
    return brls::Hint::getKeyIcon(button) + " " + label;
}

// "<icon><icon> label", for a pair such as ZL/ZR
inline std::string buttonHint(brls::ControllerButton first, brls::ControllerButton second, const std::string& label) {
    return brls::Hint::getKeyIcon(first) + brls::Hint::getKeyIcon(second) + " " + label;
}

// Header meta line: the stats, then the hints. Empty parts are skipped.
inline std::string headerMeta(const std::string& stats, std::initializer_list<std::string> hints) {
    std::string joined;
    for (const auto& h : hints) {
        if (h.empty()) continue;
        if (!joined.empty()) joined += "    ";
        joined += h;
    }
    if (stats.empty()) return joined;
    if (joined.empty()) return stats;
    return stats + "   ·   " + joined;
}

} // namespace ui::style
