#pragma once

#include "../catalog/catalog_manager.h"
#include "../download/download_manager.h"

namespace ui {

void showEntryDetails(const catalog::CatalogEntry& entry, download::DownloadManager& downloads);

} // namespace ui
