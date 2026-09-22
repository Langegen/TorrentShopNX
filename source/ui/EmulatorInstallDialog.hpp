#pragma once

#include <borealis.hpp>
#include <functional>
#include "../catalog/retro_emulator_manager.h"

namespace ui {

void showEmulatorInstallDialog(const catalog::EmulatorPackage& pkg,
                               std::function<void(bool success)> onComplete = nullptr);

void installForwarderForEmulator(const catalog::EmulatorPackage& pkg,
                                 std::function<void(bool success)> onComplete = nullptr);

void handlePostEmulatorInstallFlow(const catalog::EmulatorPackage& pkg,
                                   std::function<void()> onDone = nullptr);

} // namespace ui
