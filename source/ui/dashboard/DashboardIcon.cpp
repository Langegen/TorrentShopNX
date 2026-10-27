#include "DashboardIcon.hpp"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

constexpr float kPi = 3.14159265f;

// Drawing space is a 48x48 box; the view scales it to its size.
const NVGcolor kInk = nvgRGB(40, 44, 52);
constexpr float kLine = 2.2f;

NVGcolor rgb(int r, int g, int b) { return nvgRGB(r, g, b); }
NVGcolor white(int a = 255) { return nvgRGBA(255, 255, 255, a); }

void fillStroke(NVGcontext* vg, NVGcolor fill, float line = kLine) {
    nvgFillColor(vg, fill);
    nvgFill(vg);
    nvgStrokeColor(vg, kInk);
    nvgStrokeWidth(vg, line);
    nvgStroke(vg);
}

void strokeLine(NVGcontext* vg, float x0, float y0, float x1, float y1, NVGcolor c, float w) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, x0, y0);
    nvgLineTo(vg, x1, y1);
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, w);
    nvgStroke(vg);
}

// Soft glossy streak along the top of a shape
void highlight(NVGcontext* vg, float x0, float y, float x1) {
    strokeLine(vg, x0, y, x1, y, white(150), 2.0f);
}

// A line with a dark outline: wide ink stroke, then the colored stroke on top
void outlinedArc(NVGcontext* vg, float cx, float cy, float r, float a0, float a1, NVGcolor c) {
    for (int pass = 0; pass < 2; ++pass) {
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, r, a0, a1, NVG_CW);
        nvgStrokeColor(vg, pass == 0 ? kInk : c);
        nvgStrokeWidth(vg, pass == 0 ? 5.4f : 3.0f);
        nvgStroke(vg);
    }
}

void drawGamepad(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 4, 14, 40, 22, 11);
    fillStroke(vg, rgb(142, 108, 255));
    highlight(vg, 12, 17.5f, 22);

    // D-pad
    nvgBeginPath(vg);
    nvgMoveTo(vg, 14, 19); nvgLineTo(vg, 18, 19); nvgLineTo(vg, 18, 22); nvgLineTo(vg, 21, 22);
    nvgLineTo(vg, 21, 26); nvgLineTo(vg, 18, 26); nvgLineTo(vg, 18, 29); nvgLineTo(vg, 14, 29);
    nvgLineTo(vg, 14, 26); nvgLineTo(vg, 11, 26); nvgLineTo(vg, 11, 22); nvgLineTo(vg, 14, 22);
    nvgClosePath(vg);
    fillStroke(vg, white(), 1.6f);

    nvgBeginPath(vg);
    nvgCircle(vg, 32, 22.5f, 2.8f);
    fillStroke(vg, rgb(255, 92, 98), 1.6f);
    nvgBeginPath(vg);
    nvgCircle(vg, 37.5f, 27, 2.8f);
    fillStroke(vg, rgb(255, 202, 60), 1.6f);
}

void drawPuzzle(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, 10, 14);
    nvgLineTo(vg, 19, 14);
    nvgArc(vg, 23, 14, 4, kPi, 2 * kPi, NVG_CW);
    nvgLineTo(vg, 36, 14);
    nvgLineTo(vg, 36, 21);
    nvgArc(vg, 36, 25, 4, -kPi / 2, kPi / 2, NVG_CW);
    nvgLineTo(vg, 36, 38);
    nvgLineTo(vg, 10, 38);
    nvgClosePath(vg);
    fillStroke(vg, rgb(60, 196, 122));
    highlight(vg, 14, 18, 18);

    // Check mark: the module is in place
    nvgBeginPath(vg);
    nvgMoveTo(vg, 16, 27);
    nvgLineTo(vg, 21, 32);
    nvgLineTo(vg, 30, 22);
    nvgStrokeColor(vg, white());
    nvgStrokeWidth(vg, 3.2f);
    nvgStroke(vg);
}

void drawFolder(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, 6, 12);
    nvgLineTo(vg, 18, 12);
    nvgLineTo(vg, 22, 16);
    nvgLineTo(vg, 42, 16);
    nvgLineTo(vg, 42, 37);
    nvgLineTo(vg, 6, 37);
    nvgClosePath(vg);
    fillStroke(vg, rgb(240, 160, 50));

    // Cartridge peeking out
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 14, 15, 16, 10, 1.5f);
    fillStroke(vg, rgb(232, 236, 242), 1.6f);
    strokeLine(vg, 17, 18.5f, 27, 18.5f, rgb(228, 60, 70), 2.4f);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, 6, 21, 36, 16, 2);
    fillStroke(vg, rgb(255, 200, 92));
    highlight(vg, 10, 24.5f, 22);
}

void drawGameCards(NVGcontext* vg) {
    // Back card, tilted
    nvgSave(vg);
    nvgTranslate(vg, 29, 24);
    nvgRotate(vg, 0.28f);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, -9.5f, -13, 19, 26, 3);
    fillStroke(vg, rgb(200, 208, 220));
    nvgRestore(vg);

    // Front card
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 10, 10, 20, 28, 3);
    fillStroke(vg, rgb(245, 247, 250));
    for (int i = 0; i < 4; ++i) {
        nvgBeginPath(vg);
        nvgRect(vg, 13.5f + i * 3.6f, 12.5f, 2.2f, 4);
        nvgFillColor(vg, rgb(240, 186, 50));
        nvgFill(vg);
    }
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 13, 20, 14, 14, 2);
    fillStroke(vg, rgb(230, 0, 40), 1.6f);
    highlight(vg, 15.5f, 23, 21);
}

void drawUpdate(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgCircle(vg, 24, 24, 17);
    fillStroke(vg, rgb(255, 146, 60));
    highlight(vg, 17, 11.5f, 25);

    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    strokeLine(vg, 24, 14, 24, 29, white(), 3.4f);
    nvgBeginPath(vg);
    nvgMoveTo(vg, 18, 23.5f);
    nvgLineTo(vg, 24, 29.5f);
    nvgLineTo(vg, 30, 23.5f);
    nvgStrokeColor(vg, white());
    nvgStrokeWidth(vg, 3.4f);
    nvgStroke(vg);
    strokeLine(vg, 16.5f, 34, 31.5f, 34, white(), 3.4f);
}

void drawSdCard(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, 13, 8);
    nvgLineTo(vg, 29, 8);
    nvgLineTo(vg, 36, 15);
    nvgLineTo(vg, 36, 40);
    nvgLineTo(vg, 13, 40);
    nvgClosePath(vg);
    fillStroke(vg, rgb(70, 170, 240));

    for (int i = 0; i < 4; ++i) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, 16.5f + i * 4.2f, 10.5f, 2.6f, 6.5f, 1);
        nvgFillColor(vg, rgb(255, 210, 80));
        nvgFill(vg);
        nvgStrokeColor(vg, kInk);
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
    }
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 17, 23, 15, 12, 2);
    fillStroke(vg, white(), 1.6f);
    strokeLine(vg, 20, 27, 29, 27, rgb(70, 170, 240), 2.0f);
    strokeLine(vg, 20, 31, 26, 31, rgb(70, 170, 240), 2.0f);
}

void drawDownload(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 6, 27, 36, 13, 4);
    fillStroke(vg, rgb(58, 158, 255));
    nvgBeginPath(vg);
    nvgCircle(vg, 34.5f, 33.5f, 1.8f);
    nvgFillColor(vg, rgb(160, 255, 190));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgMoveTo(vg, 19.5f, 5);
    nvgLineTo(vg, 28.5f, 5);
    nvgLineTo(vg, 28.5f, 17);
    nvgLineTo(vg, 35, 17);
    nvgLineTo(vg, 24, 30);
    nvgLineTo(vg, 13, 17);
    nvgLineTo(vg, 19.5f, 17);
    nvgClosePath(vg);
    fillStroke(vg, white());
}

void drawDone(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgCircle(vg, 24, 24, 17);
    fillStroke(vg, rgb(52, 196, 120));
    highlight(vg, 17, 11.5f, 25);

    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgBeginPath(vg);
    nvgMoveTo(vg, 15.5f, 24.5f);
    nvgLineTo(vg, 21.5f, 30.5f);
    nvgLineTo(vg, 33, 18);
    nvgStrokeColor(vg, white());
    nvgStrokeWidth(vg, 4.0f);
    nvgStroke(vg);
}

void drawClock(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgCircle(vg, 24, 24, 17);
    fillStroke(vg, rgb(176, 188, 206));
    nvgBeginPath(vg);
    nvgCircle(vg, 24, 24, 12.5f);
    fillStroke(vg, white(), 1.6f);
    for (int i = 0; i < 4; ++i) {
        float a = i * kPi / 2;
        strokeLine(vg, 24 + std::cos(a) * 9.5f, 24 + std::sin(a) * 9.5f,
                   24 + std::cos(a) * 11.5f, 24 + std::sin(a) * 11.5f, rgb(150, 160, 178), 1.6f);
    }
    nvgLineCap(vg, NVG_ROUND);
    strokeLine(vg, 24, 24, 24, 16, kInk, 2.4f);
    strokeLine(vg, 24, 24, 30, 27.5f, kInk, 2.4f);
    nvgBeginPath(vg);
    nvgCircle(vg, 24, 24, 1.8f);
    nvgFillColor(vg, rgb(255, 92, 98));
    nvgFill(vg);
}

void drawRouter(NVGcontext* vg) {
    nvgLineCap(vg, NVG_ROUND);
    outlinedArc(vg, 24, 27, 15, -kPi * 0.78f, -kPi * 0.22f, rgb(38, 182, 168));
    outlinedArc(vg, 24, 27, 9, -kPi * 0.75f, -kPi * 0.25f, rgb(38, 182, 168));

    nvgBeginPath(vg);
    nvgRoundedRect(vg, 7, 27, 34, 12, 4);
    fillStroke(vg, rgb(38, 182, 168));
    highlight(vg, 11, 30, 20);
    for (int i = 0; i < 3; ++i) {
        nvgBeginPath(vg);
        nvgCircle(vg, 28 + i * 4.5f, 33.5f, 1.7f);
        nvgFillColor(vg, i == 0 ? rgb(170, 255, 200) : white(220));
        nvgFill(vg);
    }
}

void drawChip(NVGcontext* vg) {
    nvgLineCap(vg, NVG_ROUND);
    for (int i = 0; i < 3; ++i) {
        float p = 18 + i * 6;
        strokeLine(vg, 7, p, 12, p, kInk, 2.4f);
        strokeLine(vg, 36, p, 41, p, kInk, 2.4f);
        strokeLine(vg, p, 7, p, 12, kInk, 2.4f);
        strokeLine(vg, p, 36, p, 41, kInk, 2.4f);
    }
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 11, 11, 26, 26, 5);
    fillStroke(vg, rgb(84, 136, 240));
    highlight(vg, 15, 14.5f, 23);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 18, 18, 12, 12, 2.5f);
    fillStroke(vg, rgb(176, 205, 255), 1.6f);
}

void drawDatabase(NVGcontext* vg) {
    const NVGcolor side = rgb(150, 110, 250);
    const NVGcolor top  = rgb(205, 186, 255);
    const float layers[3] = { 29, 20, 11 };
    for (float y : layers) {
        // body
        nvgBeginPath(vg);
        nvgMoveTo(vg, 10, y);
        nvgLineTo(vg, 10, y + 7);
        nvgBezierTo(vg, 10, y + 13, 38, y + 13, 38, y + 7);
        nvgLineTo(vg, 38, y);
        nvgClosePath(vg);
        fillStroke(vg, side);
        // lid
        nvgBeginPath(vg);
        nvgEllipse(vg, 24, y, 14, 4.5f);
        fillStroke(vg, top);
    }
    highlight(vg, 15, 33, 20);
}

void drawTrash(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 19, 7, 10, 6, 2);
    fillStroke(vg, rgb(255, 170, 140), 1.8f);

    nvgBeginPath(vg);
    nvgMoveTo(vg, 12.5f, 17);
    nvgLineTo(vg, 35.5f, 17);
    nvgLineTo(vg, 32.5f, 41);
    nvgLineTo(vg, 15.5f, 41);
    nvgClosePath(vg);
    fillStroke(vg, rgb(255, 138, 100));

    nvgBeginPath(vg);
    nvgRoundedRect(vg, 9, 11.5f, 30, 6, 2.5f);
    fillStroke(vg, rgb(255, 170, 140));

    nvgLineCap(vg, NVG_ROUND);
    strokeLine(vg, 20, 22, 20.6f, 36, kInk, 1.8f);
    strokeLine(vg, 24, 22, 24, 36, kInk, 1.8f);
    strokeLine(vg, 28, 22, 27.4f, 36, kInk, 1.8f);
}

} // namespace

DashboardIcon::DashboardIcon(Kind kind, float size) : kind_(kind) {
    this->setWidth(size);
    this->setHeight(size);
    this->setShrink(0.0f);
    this->setFocusable(false);
}

void DashboardIcon::draw(NVGcontext* vg, float x, float y, float width, float height,
                         brls::Style, brls::FrameContext*) {
    const float side = std::min(width, height);
    const float scale = side / 48.0f;

    nvgSave(vg);
    nvgTranslate(vg, x + (width - side) / 2.0f, y + (height - side) / 2.0f);
    nvgScale(vg, scale, scale);
    nvgLineJoin(vg, NVG_ROUND);
    nvgLineCap(vg, NVG_ROUND);

    switch (kind_) {
        case Kind::Gamepad:   drawGamepad(vg); break;
        case Kind::Puzzle:    drawPuzzle(vg); break;
        case Kind::Folder:    drawFolder(vg); break;
        case Kind::GameCards: drawGameCards(vg); break;
        case Kind::Update:    drawUpdate(vg); break;
        case Kind::SdCard:    drawSdCard(vg); break;
        case Kind::Download:  drawDownload(vg); break;
        case Kind::Done:      drawDone(vg); break;
        case Kind::Clock:     drawClock(vg); break;
        case Kind::Router:    drawRouter(vg); break;
        case Kind::Chip:      drawChip(vg); break;
        case Kind::Database:  drawDatabase(vg); break;
        case Kind::Trash:     drawTrash(vg); break;
    }

    nvgRestore(vg);
}

} // namespace ui
