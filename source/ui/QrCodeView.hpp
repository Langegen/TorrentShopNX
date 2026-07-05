#pragma once

#include <borealis.hpp>
#include <string>

namespace ui {

class QrCodeView : public brls::View {
public:
    QrCodeView();
    ~QrCodeView() override;

    static brls::View* create() { return new QrCodeView(); }

    void setContent(const std::string& content);
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) override;

private:
    std::string content;
    bool needsUpdate = true;
};

} // namespace ui
