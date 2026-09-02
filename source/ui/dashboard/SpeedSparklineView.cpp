#include "SpeedSparklineView.hpp"
#include <algorithm>
#include <cstdio>
#include <string>

namespace ui {

static std::string formatSpeedShort(float bytes_per_sec) {
    if (bytes_per_sec <= 0.0f) return "0 KB/s";
    double mb = static_cast<double>(bytes_per_sec) / (1024.0 * 1024.0);
    char buf[32];
    if (mb >= 1.0) {
        std::snprintf(buf, sizeof(buf), "%.1f MB/s", mb);
    } else {
        double kb = static_cast<double>(bytes_per_sec) / 1024.0;
        std::snprintf(buf, sizeof(buf), "%.0f KB/s", kb);
    }
    return std::string(buf);
}

SpeedSparklineView::SpeedSparklineView() {
    this->setWidth(120.0f);
    this->setHeight(24.0f);
}

void SpeedSparklineView::setSamples(const std::deque<float>& samples) {
    samples_ = samples;
    while (samples_.size() > max_samples_) {
        samples_.pop_front();
    }
}

void SpeedSparklineView::addSpeedSample(float speed_bytes_per_sec) {
    samples_.push_back(speed_bytes_per_sec);
    while (samples_.size() > max_samples_) {
        samples_.pop_front();
    }
}

void SpeedSparklineView::clear() {
    samples_.clear();
}

void SpeedSparklineView::draw(NVGcontext* vg, float x, float y, float width, float height,
                              brls::Style style, brls::FrameContext* ctx) {
    if (width <= 10.0f || height <= 10.0f) return;

    // 1. Base card container
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 6.0f);
    nvgFillColor(vg, nvgRGBA(10, 20, 32, 160));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, 6.0f);
    nvgStrokeColor(vg, nvgRGBA(140, 180, 210, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    // 2. Subtle horizontal grid lines (33% and 66%)
    float padX = 8.0f;
    float padY = 8.0f;
    float graphW = width - padX * 2.0f;
    float graphH = height - padY * 2.0f;

    nvgBeginPath(vg);
    nvgMoveTo(vg, x + padX, y + padY + graphH * 0.33f);
    nvgLineTo(vg, x + width - padX, y + padY + graphH * 0.33f);
    nvgMoveTo(vg, x + padX, y + padY + graphH * 0.66f);
    nvgLineTo(vg, x + width - padX, y + padY + graphH * 0.66f);
    nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 18));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    if (samples_.empty()) {
        nvgFontSize(vg, 11.0f);
        nvgFontFace(vg, "default");
        nvgFillColor(vg, nvgRGBA(140, 165, 195, 180));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, x + width * 0.5f, y + height * 0.5f, "Сбор данных скорости...", nullptr);
        return;
    }

    // Calculate dynamic peak speed scale
    float peak = 1024.0f * 256.0f; // min baseline 256 KB/s
    for (float s : samples_) {
        if (s > peak) peak = s;
    }
    max_val_ = peak * 1.15f; // Add 15% headroom for aesthetic curve

    // Draw Peak Label in top-right
    std::string peakStr = "Пик: " + formatSpeedShort(peak);
    nvgFontSize(vg, 10.0f);
    nvgFontFace(vg, "default");
    nvgFillColor(vg, nvgRGBA(0, 230, 175, 220)); // Emerald
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
    nvgText(vg, x + width - padX - 2.0f, y + 4.0f, peakStr.c_str(), nullptr);

    // Compute coordinates
    std::vector<std::pair<float, float>> points;
    points.reserve(samples_.size());

    if (samples_.size() == 1) {
        float py = y + height - padY - (std::min(samples_[0], max_val_) / max_val_) * graphH;
        points.push_back({x + padX, py});
        points.push_back({x + width - padX, py});
    } else {
        float step = graphW / static_cast<float>(samples_.size() - 1);
        for (size_t i = 0; i < samples_.size(); ++i) {
            float px = x + padX + i * step;
            float py = y + height - padY - (std::min(samples_[i], max_val_) / max_val_) * graphH;
            points.push_back({px, py});
        }
    }

    // 3. Area gradient fill under the curve
    nvgBeginPath(vg);
    nvgMoveTo(vg, points.front().first, points.front().second);
    for (size_t i = 1; i < points.size(); ++i) {
        nvgLineTo(vg, points[i].first, points[i].second);
    }
    nvgLineTo(vg, points.back().first, y + height - padY);
    nvgLineTo(vg, points.front().first, y + height - padY);
    nvgClosePath(vg);

    NVGpaint areaPaint = nvgLinearGradient(vg, x, y + padY, x, y + height - padY,
                                           nvgRGBA(0, 224, 165, 85),
                                           nvgRGBA(0, 224, 165, 0));
    nvgFillPaint(vg, areaPaint);
    nvgFill(vg);

    // 4. Vibrant glowing neon line stroke
    nvgBeginPath(vg);
    nvgMoveTo(vg, points.front().first, points.front().second);
    for (size_t i = 1; i < points.size(); ++i) {
        nvgLineTo(vg, points[i].first, points[i].second);
    }
    nvgStrokeColor(vg, nvgRGBA(0, 245, 190, 255)); // Bright Emerald
    nvgStrokeWidth(vg, 2.2f);
    nvgStroke(vg);

    // 5. Glowing dot on latest sample point
    float lastX = points.back().first;
    float lastY = points.back().second;

    // Outer glow ring
    nvgBeginPath(vg);
    nvgCircle(vg, lastX, lastY, 5.0f);
    nvgFillColor(vg, nvgRGBA(0, 245, 190, 90));
    nvgFill(vg);

    // White core
    nvgBeginPath(vg);
    nvgCircle(vg, lastX, lastY, 2.8f);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
    nvgFill(vg);
}

} // namespace ui
