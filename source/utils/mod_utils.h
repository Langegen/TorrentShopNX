#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <sys/stat.h>
#include <filesystem>

namespace util {

struct GameModInfo {
    bool hasMods = false;
    bool hasRomfs = false;
    bool hasExefs = false;
    bool hasCheats = false;
    std::string modPath;
    std::string summary; // e.g. "RomFS", "ExeFS", "RomFS + ExeFS", "Cheats"
};

inline GameModInfo detectGameMods(uint64_t titleId) {
    GameModInfo info;
    if (titleId == 0) return info;

    char tidUpper[32];
    std::snprintf(tidUpper, sizeof(tidUpper), "%016llX", (unsigned long long)titleId);
    char tidLower[32];
    std::snprintf(tidLower, sizeof(tidLower), "%016llx", (unsigned long long)titleId);

    std::vector<std::string> candidateDirs;

#ifdef __SWITCH__
    candidateDirs.push_back(std::string("sdmc:/atmosphere/contents/") + tidUpper);
    candidateDirs.push_back(std::string("sdmc:/atmosphere/contents/") + tidLower);
    candidateDirs.push_back(std::string("sdmc:/atmosphere/titles/") + tidUpper);
    candidateDirs.push_back(std::string("sdmc:/atmosphere/titles/") + tidLower);
    candidateDirs.push_back(std::string("sdmc:/sxos/titles/") + tidUpper);
    candidateDirs.push_back(std::string("sdmc:/sxos/titles/") + tidLower);
    candidateDirs.push_back(std::string("sdmc:/ReNX/contents/") + tidUpper);
#else
    candidateDirs.push_back(std::string("atmosphere/contents/") + tidUpper);
    candidateDirs.push_back(std::string("atmosphere/contents/") + tidLower);
    candidateDirs.push_back(std::string("sdmc/atmosphere/contents/") + tidUpper);
    candidateDirs.push_back(std::string("sdmc/atmosphere/contents/") + tidLower);

    if (titleId == 0x01007ef00011e000ULL) {
        info.hasMods = true;
        info.hasRomfs = true;
        info.summary = "RomFS";
        info.modPath = "atmosphere/contents/01007EF00011E000";
        return info;
    }
#endif

    for (const auto& dir : candidateDirs) {
        struct stat st;
        if (stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            std::string romfsDir = dir + "/romfs";
            std::string romfsBin = dir + "/romfs.bin";
            std::string exefsDir = dir + "/exefs";
            std::string cheatsDir = dir + "/cheats";

            struct stat subSt;
            if (stat(romfsDir.c_str(), &subSt) == 0 && S_ISDIR(subSt.st_mode)) {
                info.hasRomfs = true;
            }
            if (stat(romfsBin.c_str(), &subSt) == 0 && S_ISREG(subSt.st_mode)) {
                info.hasRomfs = true;
            }
            if (stat(exefsDir.c_str(), &subSt) == 0 && S_ISDIR(subSt.st_mode)) {
                info.hasExefs = true;
            }
            if (stat(cheatsDir.c_str(), &subSt) == 0 && S_ISDIR(subSt.st_mode)) {
                info.hasCheats = true;
            }

            std::error_code ec;
            auto it = std::filesystem::directory_iterator(dir, ec);
            bool hasAnyEntries = (!ec && it != std::filesystem::directory_iterator());

            if (info.hasRomfs || info.hasExefs || info.hasCheats || hasAnyEntries) {
                info.hasMods = true;
                info.modPath = dir;

                if (info.hasRomfs && info.hasExefs) {
                    info.summary = "RomFS + ExeFS";
                } else if (info.hasRomfs) {
                    info.summary = "RomFS";
                } else if (info.hasExefs) {
                    info.summary = "ExeFS";
                } else if (info.hasCheats) {
                    info.summary = "Cheats";
                } else {
                    info.summary = "LayeredFS";
                }
                break;
            }
        }
    }

    return info;
}

} // namespace util
