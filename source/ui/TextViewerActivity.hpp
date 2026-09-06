#pragma once

#include <borealis.hpp>
#include <string>

namespace ui {

class TextViewerActivity : public brls::Activity {
public:
    TextViewerActivity(const std::string& filePath, const std::string& fileName = "");
    ~TextViewerActivity() override = default;

    brls::View* createContentView() override;
    void onContentAvailable() override;

private:
    std::string filePath_;
    std::string fileName_;
    brls::ScrollingFrame* scrollingFrame_ = nullptr;
    brls::Label* textLabel_ = nullptr;
};

} // namespace ui
