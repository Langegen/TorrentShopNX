/*
    Copyright 2020-2021 natinusala

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <borealis/core/application.hpp>
#include <borealis/core/assets.hpp>
#include <borealis/core/i18n.hpp>
#include <borealis/core/util.hpp>
#ifdef USE_BOOST_FILESYSTEM
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;
#elif __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#elif __has_include("experimental/filesystem")
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
#error "Failed to include <filesystem> header!"
#endif
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#ifndef BRLS_I18N_PREFIX
#define BRLS_I18N_PREFIX ""
#endif

namespace brls
{

static nlohmann::json defaultLocale = {};
static nlohmann::json currentLocale = {};

static void loadLocale(std::string locale, nlohmann::json* target)
{
    if (locale.empty())
        return;
#ifdef USE_LIBROMFS
    auto localePath = romfs::list("i18n/" + locale);
    if (localePath.empty())
    {
        Logger::error("Cannot load locale {}: directory i18n/{} doesn't exist", locale, locale);
        return;
    }
    for (auto& entry : localePath)
    {
        std::string path = entry.string();
        std::string name = entry.filename().string();
        if (!endsWith(name, ".json"))
            continue;

        (*target)[name.substr(0, name.length() - 5)] = nlohmann::json::parse(romfs::get(path).string());
    }
#else
    std::string localePath = BRLS_ASSET("i18n/" + locale);

    if (!fs::exists(localePath))
    {
        std::string fallbackLocale;
        if (startsWith(locale, "zh"))
            fallbackLocale = LOCALE_ZH_HANS;
        else if (startsWith(locale, "es"))
            fallbackLocale = LOCALE_ES;
        else if (startsWith(locale, "pt"))
            fallbackLocale = LOCALE_PT_BR;
        else if (startsWith(locale, "fr"))
            fallbackLocale = LOCALE_FR;
        else if (startsWith(locale, "de"))
            fallbackLocale = LOCALE_DE;
        else if (startsWith(locale, "it"))
            fallbackLocale = LOCALE_IT;
        else if (startsWith(locale, "ru"))
            fallbackLocale = LOCALE_RU;
        else if (startsWith(locale, "ja"))
            fallbackLocale = LOCALE_JA;
        else if (startsWith(locale, "en"))
            fallbackLocale = LOCALE_EN_US;

        if (!fallbackLocale.empty() && fallbackLocale != locale)
        {
            std::string fallbackPath = BRLS_ASSET("i18n/" + fallbackLocale);
            if (fs::exists(fallbackPath))
            {
                locale = fallbackLocale;
                localePath = fallbackPath;
            }
        }
    }

    if (!fs::exists(localePath))
    {
        Logger::error("Cannot load locale {}: directory {} doesn't exist", locale, localePath);
        return;
    }
    else if (!fs::is_directory(localePath))
    {
        Logger::error("Cannot load locale {}: {} isn't a directory", locale, localePath);
        return;
    }

    // Iterate over all JSON files in the directory
    for (const fs::directory_entry& entry : fs::directory_iterator(localePath))
    {
        if (fs::is_directory(entry))
            continue;

        std::string name = entry.path().filename().string();

        if (!endsWith(name, ".json"))
            continue;

        std::string path = entry.path().string();

        nlohmann::json strings;

        std::ifstream jsonStream;
        jsonStream.open(path);

        try
        {
            jsonStream >> strings;
        }
        catch (const std::exception& e)
        {
            Logger::error("Error while loading \"{}\": {}", path, e.what());
        }

        jsonStream.close();

        (*target)[name.substr(0, name.length() - 5)] = strings;
    }
#endif /* USE_LIBROMFS */
}

void reloadTranslations(std::string locale)
{
    currentLocale = nlohmann::json::object();

    if (defaultLocale.empty())
        loadLocale(LOCALE_DEFAULT, &defaultLocale);

    std::string currentLocaleName = locale;
    if (currentLocaleName.empty() || currentLocaleName == LOCALE_AUTO)
    {
        currentLocaleName = Application::getLocale();
    }

    if (currentLocaleName != LOCALE_DEFAULT)
        loadLocale(currentLocaleName, &currentLocale);
    else
        currentLocale = defaultLocale;
}

void loadTranslations()
{
    loadLocale(LOCALE_DEFAULT, &defaultLocale);
    reloadTranslations(Application::getLocale());
}

namespace internal
{
    std::string getRawStr(std::string stringName)
    {
        nlohmann::json::json_pointer pointer;

        try
        {
            pointer = nlohmann::json::json_pointer("/" + std::string(BRLS_I18N_PREFIX) + stringName);
        }
        catch (const std::exception& e)
        {
            Logger::error("Error while getting string \"{}\": {}", stringName, e.what());
            return stringName;
        }

        // First look for translated string in current locale
        try
        {
            return currentLocale[pointer].get<std::string>();
        }
        catch (...)
        {
        }

        // Then look for default locale
        try
        {
            return defaultLocale[pointer].get<std::string>();
        }
        catch (...)
        {
        }

        // Fallback to returning the string name
        return stringName;
    }
} // namespace internal

inline namespace literals
{
    std::string operator"" _i18n(const char* str, size_t len)
    {
        return internal::getRawStr(std::string(str, len));
    }

} // namespace literals

} // namespace brls
