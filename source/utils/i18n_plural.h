#pragma once

#include <string>

namespace util {

// Count with the right word form for the UI language: "1 игра", "3 игры",
// "5 игр", "1 game", "2 games".
//
// Looks up "<baseKey>_one", "_few", "_many" or "_other" and fills {0} with n.
// Every language file must define all four forms of a plural key: borealis
// falls back to the English string, not to another form, when a key is missing.
std::string pluralize(const std::string& baseKey, long long n);

} // namespace util
