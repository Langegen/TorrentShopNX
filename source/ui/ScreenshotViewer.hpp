#pragma once
#include <borealis.hpp>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ui {

// Full-screen screenshot gallery. The current screenshot and its neighbours
// are loaded into their own layers, so switching shows a ready picture at
// once; while a new one is still loading the previous one stays on screen,
// and "Loading…" appears only after a short wait (no blank placeholder
// flashing between pictures).
class ScreenshotViewer : public brls::Activity {
public:
    ScreenshotViewer(const std::vector<std::string>& urls, size_t startIndex = 0);
    ~ScreenshotViewer();

    brls::View* createContentView() override;
    void onContentAvailable() override;

private:
    struct Slot;

    void goTo(size_t index);
    void nextImage();
    void prevImage();
    void ensureSlot(size_t index, int priority);
    void dropFarSlots();
    void updateChrome();
    // Runs every frame: picks which layer is visible and whether to show
    // the loading / error text.
    void refresh();

    std::vector<std::string> urls_;
    size_t currentIndex_;
    static constexpr size_t kNone = static_cast<size_t>(-1);
    size_t shownIndex_ = kNone;   // screenshot on screen right now
    std::chrono::steady_clock::time_point waitSince_;

    std::map<size_t, std::unique_ptr<Slot>> slots_;

    brls::Box* root_ = nullptr;
    brls::Label* counterLabel_ = nullptr;
    brls::Label* loadingLabel_ = nullptr;
    brls::Label* failedLabel_ = nullptr;
    brls::Box* prevBtn_ = nullptr;
    brls::Box* nextBtn_ = nullptr;
};

} // namespace ui
