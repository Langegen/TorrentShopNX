#pragma once

#include "../catalog/catalog_manager.h"
#include "../download/download_manager.h"

namespace ui {

void showCatalogBrowser(const std::vector<catalog::CatalogEntry>& entries, download::DownloadManager& downloads);

} // namespace ui
