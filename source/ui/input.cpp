#include "input.h"

#include <array>
#include <chrono>

#include <switch/services/hid.h>

PadState g_pad;

namespace {

using Clock = std::chrono::steady_clock;

struct RepeatState {
    u64 button = 0;
    bool active = false;
    Clock::time_point next_fire{};
};

constexpr auto kInitialRepeatDelay = std::chrono::milliseconds(350);
constexpr auto kRepeatInterval = std::chrono::milliseconds(80);

std::array<RepeatState, 6> g_repeat_states{{
    {HidNpadButton_Up, false, {}},
    {HidNpadButton_Down, false, {}},
    {HidNpadButton_Left, false, {}},
    {HidNpadButton_Right, false, {}},
    {HidNpadButton_L, false, {}},
    {HidNpadButton_R, false, {}},
}};

u64 g_last_buttons_down = 0;
u64 g_last_buttons_held = 0;

u64 remapSticksToDpad(u64 buttons) {
    if (buttons & HidNpadButton_StickLUp)   buttons |= HidNpadButton_Up;
    if (buttons & HidNpadButton_StickLDown) buttons |= HidNpadButton_Down;
    if (buttons & HidNpadButton_StickLLeft) buttons |= HidNpadButton_Left;
    if (buttons & HidNpadButton_StickLRight) buttons |= HidNpadButton_Right;

    if (buttons & HidNpadButton_StickRUp)   buttons |= HidNpadButton_Up;
    if (buttons & HidNpadButton_StickRDown) buttons |= HidNpadButton_Down;
    if (buttons & HidNpadButton_StickRLeft) buttons |= HidNpadButton_Left;
    if (buttons & HidNpadButton_StickRRight) buttons |= HidNpadButton_Right;

    return buttons;
}

} // namespace

void inputInit() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    g_last_buttons_down = 0;
    g_last_buttons_held = 0;
    for (auto& state : g_repeat_states) {
        state.active = false;
        state.next_fire = Clock::time_point{};
    }
}

u64 inputDown() {
    padUpdate(&g_pad);
    g_last_buttons_down = remapSticksToDpad(padGetButtonsDown(&g_pad));
    g_last_buttons_held = remapSticksToDpad(padGetButtons(&g_pad));

    const auto now = Clock::now();
    for (auto& state : g_repeat_states) {
        if (g_last_buttons_down & state.button) {
            state.active = true;
            state.next_fire = now + kInitialRepeatDelay;
        } else if (!(g_last_buttons_held & state.button)) {
            state.active = false;
            state.next_fire = Clock::time_point{};
        }
    }

    return g_last_buttons_down;
}

u64 inputRepeat(u64 mask) {
    const auto now = Clock::now();
    u64 repeated = 0;

    for (auto& state : g_repeat_states) {
        if (!(mask & state.button)) continue;
        if (!state.active) continue;
        if (!(g_last_buttons_held & state.button)) continue;
        if (g_last_buttons_down & state.button) continue;
        if (state.next_fire.time_since_epoch().count() == 0) continue;
        if (now < state.next_fire) continue;

        repeated |= state.button;
        do {
            state.next_fire += kRepeatInterval;
        } while (now >= state.next_fire);
    }

    return repeated;
}
