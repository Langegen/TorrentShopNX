#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include "../utils/log.h"

namespace catalog {

constexpr const char* kGitLabCatalogBaseUrl = "https://gitlab.com/i3sey/catalog-data/-/raw/main/";
constexpr const char* kGitVerseCatalogBaseUrl = "https://gitverse.ru/api/repos/i3sey/catalog-data/raw/branch/main/";

// Returns ordered fallback URLs for a given catalog file / URL:
// 1. Primary URL (GitHub / custom user URL)
// 2. GitLab mirror (https://gitlab.com/i3sey/catalog-data/)
// 3. GitVerse mirror (https://gitverse.ru/i3sey/catalog-data)
inline std::vector<std::string> getFallbackUrls(const std::string& originalUrl) {
    std::vector<std::string> urls;
    if (originalUrl.empty()) return urls;

    urls.push_back(originalUrl);

    // Extract filename from the end of the URL
    size_t lastSlash = originalUrl.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos) ? originalUrl.substr(lastSlash + 1) : originalUrl;

    // Remove query params if any
    size_t queryPos = filename.find('?');
    if (queryPos != std::string::npos) {
        filename = filename.substr(0, queryPos);
    }

    if (!filename.empty() && filename.find(".json") != std::string::npos) {
        std::string gitlabUrl = std::string(kGitLabCatalogBaseUrl) + filename;
        if (std::find(urls.begin(), urls.end(), gitlabUrl) == urls.end()) {
            urls.push_back(gitlabUrl);
        }

        std::string gitverseUrl = std::string(kGitVerseCatalogBaseUrl) + filename;
        if (std::find(urls.begin(), urls.end(), gitverseUrl) == urls.end()) {
            urls.push_back(gitverseUrl);
        }
    }

    return urls;
}

} // namespace catalog
