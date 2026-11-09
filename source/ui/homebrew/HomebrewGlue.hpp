#pragma once

// What the homebrew store screens need from the rest of the app. The app's
// implementation is HomebrewGlue.cpp; the PC window for testing the screens
// (pctest/homebrew_ui) has its own, so the screens build without the torrent
// engine, libarchive and the image cache.

#include <borealis.hpp>

#include <memory>
#include <string>

namespace ui::hb {

// Icon or screenshot from a URL, cached; placeholder until it arrives.
void loadImage(brls::Image* img, const std::string& url, const std::shared_ptr<bool>& token, bool full_size = false,
               int row = -1, int col = -1);

void haptic(int style);

// "homebrew" or "retro": the tab the section opens on.
std::string currentSection();
void rememberSection(const std::string& tab);

// Switch the section to the retro tab (replaces the homebrew screen).
void showRetroSection();

// Today, "yyyy-mm-dd" (for the "New" badge).
std::string today();

// After an install or removal: refresh counters elsewhere in the app.
void packagesChanged();

// Readable text color on a filled background: dark on light accents (the
// graphite theme's accent is near white), white on the rest.
inline NVGcolor textOn(NVGcolor bg) {
    const float lum = 0.299f * bg.r + 0.587f * bg.g + 0.114f * bg.b;
    return lum > 0.62f ? nvgRGB(24, 26, 32) : nvgRGB(255, 255, 255);
}

// Replace the top activity with next on the next frame (never from inside the
// old activity's own action handler: popActivity deletes it immediately).
inline void replaceTopActivity(brls::Activity* next) {
    brls::sync([next]() {
        brls::Application::popActivity(brls::TransitionAnimation::NONE, [next]() {
            brls::Application::pushActivity(next, brls::TransitionAnimation::NONE);
        });
    });
}

}  // namespace ui::hb
