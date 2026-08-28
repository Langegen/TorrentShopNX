#pragma once

#include <borealis.hpp>
#include <deque>

namespace ui {

class SpeedSparklineView : public brls::View {
public:
    SpeedSparklineView();

    void addSpeedSample(float speed_bytes_per_sec);
    void clear();

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

private:
    std::deque<float> samples_;
    size_t max_samples_ = 36;
    float max_val_ = 1024.0f * 1024.0f; // 1 MB/s min baseline
};

} // namespace ui
