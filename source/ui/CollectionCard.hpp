#pragma once

#include <borealis.hpp>
#include <string>
#include <vector>
#include <functional>

namespace ui {

class CollectionCard : public brls::Box {
public:
    CollectionCard(const std::string& icon_type,
                   const std::string& title,
                   const std::string& desc,
                   const std::string& count_text,
                   NVGcolor icon_color,
                   std::function<void()> on_click);

    void setCountText(const std::string& text);

    void onFocusGained() override;
    void onFocusLost() override;
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    void drawIconGlyph(NVGcontext* vg, float bx, float by, float bw, float bh);

    std::string icon_type_;
    std::string title_;
    std::string count_text_;
    NVGcolor icon_color_;
    std::function<void()> on_click_;

    brls::Label* title_label_ = nullptr;
    brls::Label* desc_label_ = nullptr;
    brls::Label* count_label_ = nullptr;

    float scale_ = 1.0f;
    float glow_ = 0.0f;
};

} // namespace ui
