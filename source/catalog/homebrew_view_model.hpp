#pragma once

// What the homebrew store screen shows: category tabs, search, sort, the text
// to display. No UI code, so it is tested in pctest/test_homebrew_catalog.cpp.

#include "homebrew_catalog.hpp"

#include <array>
#include <string>
#include <vector>

namespace homebrew {

enum class Category { All, Apps, Games, Emulators, Overlays, Sysmodules, Themes, Installed, Count };
enum class SortMode { Popular, Updated, Name, Count };

constexpr size_t kCategoryCount = static_cast<size_t>(Category::Count);

struct ViewItem {
    const Package* pkg = nullptr;  // points into the PackageList the view was built from
    Status status = Status::NotInstalled;
    bool untracked = false;  // installed by hand, version unknown
    bool is_new = false;     // auto-found in the last 14 days
};

// A package can be in several tabs: sys-clk is an app, an overlay and a sysmodule.
bool inCategory(const Package& pkg, Category c, Status status);

// Filtered and sorted. query matches title, name, author and descriptions
// (case-insensitive for Latin letters). Installed always lists updates first.
std::vector<ViewItem> buildView(const PackageList& pkgs, const InstalledMap& installed, Category c, SortMode sort,
                                const std::string& query, const std::string& today);

// How many packages each tab has (no query); Installed counts installed ones.
std::array<int, kCategoryCount> countByCategory(const PackageList& pkgs, const InstalledMap& installed);

// Description for the UI language ("ru", "en-US", "zh-Hans"): the index's
// translation for that language, else the original.
std::string displayDescription(const Package& pkg, const std::string& locale);

// "12.3 MB" with the given unit names {B, KB, MB, GB}; "" for 0 or less.
std::string formatSize(int64_t bytes, const std::array<std::string, 4>& units);

// "2026-11-09" -> "09.11.2026"; other strings unchanged.
std::string displayDate(const std::string& iso);

// Release notes from GitHub are Markdown: headings, **bold**, `code`, list
// marks and [links](url) become plain text ("• " for list items).
std::string plainText(const std::string& markdown);

// fortheusers writes "n/a" for an unknown license.
bool isKnownLicense(const std::string& license);

}  // namespace homebrew
