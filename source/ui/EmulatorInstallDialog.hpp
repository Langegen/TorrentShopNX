#pragma once

#include <borealis.hpp>
#include <functional>
#include "../catalog/retro_emulator_manager.h"

namespace ui {

void showEmulatorInstallDialog(const catalog::EmulatorPackage& pkg,
                               std::function<void(bool success)> onComplete = nullptr);

} // namespace ui
