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

class QrDialog : public brls::Box {
public:
    QrDialog(const std::string& title, const std::string& url, const std::string& hint = "");

    static void open(const std::string& title, const std::string& url, const std::string& hint = "");

    void dismissDialog();

    brls::Button* getOkButton() const { return btnOk_; }

private:
    brls::Button* btnOk_ = nullptr;
};

} // namespace ui
