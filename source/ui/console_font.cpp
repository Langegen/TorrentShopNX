#include "console_font.h"

#include <switch.h>

#include <fstream>
#include <vector>

#include "../utils/log.h"

namespace ui {

namespace {

constexpr size_t kExpectedFontSize = 0x2000;

std::vector<uint8_t> g_console_font_data;
ConsoleFont g_console_font = {};

} // namespace

bool installConsoleFont() {
    std::ifstream in("romfs:/console_cyrillic_font.bin", std::ios::binary);
    if (!in.is_open()) {
        util::logLine("ui: console font asset not found");
        return false;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() != kExpectedFontSize) {
        util::logLine("ui: invalid console font size=" + std::to_string(data.size()));
        return false;
    }

    g_console_font_data = std::move(data);
    g_console_font.gfx = g_console_font_data.data();
    g_console_font.asciiOffset = 0;
    g_console_font.numChars = 256;
    g_console_font.tileWidth = 16;
    g_console_font.tileHeight = 16;

    consoleSetFont(consoleGetDefault(), &g_console_font);
    util::logLine("ui: console font installed");
    return true;
}

} // namespace ui
