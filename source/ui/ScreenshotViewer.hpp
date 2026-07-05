#pragma once
#include <borealis.hpp>
#include <vector>
#include <string>
#include <memory>

namespace ui {

class ScreenshotViewer : public brls::Activity {
public:
    ScreenshotViewer(const std::vector<std::string>& urls, size_t startIndex = 0);
    ~ScreenshotViewer();

    brls::View* createContentView() override;
    void onContentAvailable() override;

private:
    void loadCurrent();

    std::vector<std::string> urls_;
    size_t currentIndex_;
    brls::Image* image_;
    std::shared_ptr<bool> imageToken_;
};

} // namespace ui
