#pragma once

#include <functional>

namespace ui {

void showRetroCatalogUpdateDialog(std::function<void(int updatedCount)> onComplete = nullptr);

} // namespace ui
