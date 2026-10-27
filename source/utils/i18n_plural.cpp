#include "i18n_plural.h"

#include <borealis.hpp>

namespace util {

static const char* pluralCategory(const std::string& locale, long long n) {
    if (n < 0) n = -n;
    if (locale.rfind("ru", 0) == 0 || locale.rfind("uk", 0) == 0) {
        long long m10 = n % 10, m100 = n % 100;
        if (m10 == 1 && m100 != 11) return "one";
        if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return "few";
        return "many";
    }
    if (locale.rfind("ja", 0) == 0 || locale.rfind("zh", 0) == 0) return "other";
    if (locale.rfind("fr", 0) == 0 || locale.rfind("pt", 0) == 0) return (n <= 1) ? "one" : "other";
    return (n == 1) ? "one" : "other";
}

std::string pluralize(const std::string& baseKey, long long n) {
    const char* cat = pluralCategory(brls::Application::getLocale(), n);
    return brls::getStr(baseKey + "_" + cat, std::to_string(n));
}

} // namespace util
