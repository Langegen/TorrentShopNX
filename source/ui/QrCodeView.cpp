#include "QrCodeView.hpp"
#include "../utils/qrcodegen.hpp"
#include <vector>
#include <algorithm>

using qrcodegen::QrCode;
using qrcodegen::QrSegment;

namespace ui {

QrCodeView::QrCodeView() {
    this->setWidth(256);
    this->setHeight(256);
}

QrCodeView::~QrCodeView() {
}

void QrCodeView::setContent(const std::string& text) {
    this->content = text;
    this->needsUpdate = true;
}

void QrCodeView::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) {
    if (content.empty()) {
        return;
    }

    try {
        QrCode qr = QrCode::encodeText(content.c_str(), QrCode::Ecc::LOW);

        int size = qr.getSize();
        int margin = 2; // quiet zone
        int fullSize = size + margin * 2;
        
        float scale = std::min(width / fullSize, height / fullSize);
        
        float actualWidth = fullSize * scale;
        float actualHeight = fullSize * scale;
        float offsetX = x + (width - actualWidth) / 2.0f;
        float offsetY = y + (height - actualHeight) / 2.0f;

        // Draw white background
        nvgBeginPath(vg);
        nvgRect(vg, offsetX, offsetY, actualWidth, actualHeight);
        nvgFillColor(vg, nvgRGB(255, 255, 255));
        nvgFill(vg);

        // Draw black squares
        nvgFillColor(vg, nvgRGB(0, 0, 0));
        
        for (int yqr = 0; yqr < size; yqr++) {
            for (int xqr = 0; xqr < size; xqr++) {
                if (qr.getModule(xqr, yqr)) {
                    float rectX = offsetX + (xqr + margin) * scale;
                    float rectY = offsetY + (yqr + margin) * scale;
                    
                    nvgBeginPath(vg);
                    // Draw slightly larger to avoid anti-aliasing gaps between modules
                    nvgRect(vg, rectX, rectY, scale + 0.5f, scale + 0.5f);
                    nvgFill(vg);
                }
            }
        }
    } catch (...) {
        // Fallback or error handling if encoding fails
    }
}

} // namespace ui
