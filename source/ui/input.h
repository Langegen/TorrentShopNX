#pragma once

#include <switch.h>
#include <switch/runtime/pad.h>

extern PadState g_pad;

void inputInit();
u64 inputDown();
u64 inputRepeat(u64 mask);
