#include "SpeedSparklineView.hpp"
#include <algorithm>

namespace ui {

SpeedSparklineView::SpeedSparklineView() {
    this->setWidth(120.0f);
    this->setHeight(24.0f);
    
    // Seed initial flat samples
    for (size_t i = 0; i < max_samples_; ++i) {
        samples_.push_back(0.0f);
    }
}

void SpeedSparklineView::addSpeedSample(float speed_bytes_per_sec) {
    samples_.push_back(speed_bytes_per_sec);
    while (samples_.size() > max_samples_) {
        samples_.pop_front();
    }
    
    // Calculate running max with smooth decay
    float peak = 1024.0f * 512.0f; // min 512 KB/s scale
    for (float s : samples_) {
        if (s > peak) peak = s;
    }
    max_val_ = max_val_ * 0.9f + peak * 0.1f;
    if (max_val_ < 1024.0f) max_val_ = 1024.0f;
}

void SpeedSparklineView::clear() {
    samples_.clear();
    for (size_t i = 0; i < max_samples_; ++i) {
        samples_.push_back(0.0f);
    }
}

void SpeedSparklineView::draw(NVGcontext* vg, float x, float y, float width, float height,
                              brls::Style style, brls::FrameContext* ctx) {
    if (samples_.size() < 2 || width <= 10.0f || height <= 10.0f) return;

    // 1. Transparent subtle baseline
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 4.0f);
    nvgFillColor(vg, nvgRGBA(12, 18, 28, 120));
    nvgFill(vg);

    // Subtle horizontal baseline grid
    nvgBeginPath(vg);
    nvgMoveTo(vg, x + 4.0f, y + height * 0.5f);
    nvgLineTo(vg, x + width - 4.0f, y + height * 0.5f);
    nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 15));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    float padX = 6.0f;
    float padY = 5.0f;
    float graphW = width - padX * 2.0f;
    float graphH = height - padY * 2.0f;
    float step = graphW / static_cast<float>(samples_.size() - 1);

    // 2. Area gradient fill below line
    nvgBeginPath(vg);
    float startX = x + padX;
    float startY = y + height - padY - (std::min(samples_[0], max_val_) / max_val_) * graphH;
    nvgMoveTo(vg, startX, startY);

    for (size_t i = 1; i < samples_.size(); ++i) {
        float px = x + padX + i * step;
        float py = y + height - padY - (std::min(samples_[i], max_val_) / max_val_) * graphH;
        nvgLineTo(vg, px, py);
    }

    nvgLineTo(vg, x + width - padX, y + height - padY);
    nvgLineTo(vg, x + padX, y + height - padY);
    nvgClosePath(vg);

    NVGpaint areaPaint = nvgLinearGradient(vg, x, y + padY, x, y + height,
                                           nvgRGBA(0, 229, 255, 100),
                                           nvgRGBA(0, 229, 255, 5));
    nvgFillPaint(vg, areaPaint);
    nvgFill(vg);

    // 3. Glowing neon line stroke
    nvgBeginPath(vg);
    nvgMoveTo(vg, startX, startY);
    for (size_t i = 1; i < samples_.size(); ++i) {
        float px = x + padX + i * step;
        float py = y + height - padY - (std::min(samples_[i], max_val_) / max_val_) * graphH;
        nvgLineTo(vg, px, py);
    }
    nvgStrokeColor(vg, nvgRGBA(0, 229, 255, 230));
    nvgStrokeWidth(vg, 2.0f);
    nvgStroke(vg);

    // 4. Dot on latest sample point
    float lastX = x + padX + (samples_.size() - 1) * step;
    float lastY = y + height - padY - (std::min(samples_.back(), max_val_) / max_val_) * graphH;
    nvgBeginPath(vg);
    nvgCircle(vg, lastX, lastY, 2.5f);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
    nvgFill(vg);
}

} // namespace ui
