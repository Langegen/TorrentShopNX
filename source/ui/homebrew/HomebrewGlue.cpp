#include "HomebrewGlue.hpp"

#include "../../GameData.hpp"
#include "../../config/config.h"
#include "../../utils/switch_utils.h"
#include "../RetroConsolesView.hpp"

#include <ctime>

namespace ui::hb {

void loadImage(brls::Image* img, const std::string& url, const std::shared_ptr<bool>& token, bool full_size, int row,
               int col) {
    setImageFromHTTPS(img, url, token, "romfs:/img/cover_blank.png", false, "", row, col, 0, full_size);
}

void haptic(int style) { util::triggerHapticFeedback(style); }

std::string currentSection() { return config::ConfigManager::instance().getHomebrewSection(); }

void rememberSection(const std::string& tab) { config::ConfigManager::instance().setHomebrewSection(tab); }

void showRetroSection() {
    rememberSection("retro");
    replaceTopActivity(new RetroConsolesView());
}

std::string today() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

void packagesChanged() {}

}  // namespace ui::hb
