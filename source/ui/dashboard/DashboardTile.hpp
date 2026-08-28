#pragma once

#include <borealis.hpp>
#include <functional>
#include <string>

namespace ui {

class DashboardTile : public brls::Box {
public:
    DashboardTile(int index, const std::string& icon_res, const std::string& title,
                  std::function<void(int)> on_focus, std::function<void(int)> on_click);

    void setBadge(const std::string& text);
    void setTitle(const std::string& title);
    int getIndex() const { return index_; }

    void onFocusGained() override;
    void onFocusLost() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    int index_;
    std::string title_;
    std::function<void(int)> on_focus_;
    std::function<void(int)> on_click_;

    brls::Box* card_box_ = nullptr;
    brls::Image* icon_view_ = nullptr;
    brls::Label* sub_badge_ = nullptr;
    brls::Label* title_label_ = nullptr;

    float scale_ = 1.0f;
    float glow_  = 0.0f;
};

} // namespace ui
