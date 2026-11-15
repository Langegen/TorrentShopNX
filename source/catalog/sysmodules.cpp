#include "sysmodules.hpp"

#include "homebrew_fs.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <map>

namespace homebrew {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool isHexTid(const std::string& s) {
    if (s.size() != 16) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string contentsDir(const std::string& sd_root, const std::string& tid) {
    return fsx::join(fsx::join(sd_root, "atmosphere/contents"), tid);
}

}  // namespace

unsigned long long parseTitleId(const std::string& tid) {
    if (!isHexTid(tid)) return 0;
    return std::strtoull(tid.c_str(), nullptr, 16);
}

std::vector<std::string> sysmoduleTids(const std::vector<ManifestEntry>& files) {
    return analyzeFiles(files).sysmodule_tids;
}

std::vector<SysmoduleInfo> scanSysmodules(const std::string& sd_root, const std::vector<Package>* pkgs,
                                          const std::map<std::string, Installed>* installed) {
    // Which store package installs which module: the registry first (it is
    // what is on the card), then the file lists of the packages.
    std::map<std::string, const Package*> owner;
    if (pkgs && installed) {
        std::map<std::string, const Package*> byName;
        for (const Package& p : *pkgs) byName.emplace(fsx::lower(p.name), &p);
        for (const auto& kv : *installed) {
            auto it = byName.find(kv.first);
            if (it == byName.end()) continue;
            for (const auto& t : sysmoduleTids(kv.second.manifest)) owner.emplace(t, it->second);
        }
    }
    if (pkgs)
        for (const Package& p : *pkgs)
            for (const auto& t : sysmoduleTids(p.files)) owner.emplace(t, &p);

    std::vector<SysmoduleInfo> out;
    const std::string base = fsx::join(sd_root, "atmosphere/contents");
    DIR* d = opendir(base.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        const std::string entry = e->d_name;
        if (!isHexTid(entry)) continue;
        const std::string dir = fsx::join(base, entry);
        if (!fsx::fileExists(fsx::join(dir, "exefs.nsp"))) continue;  // game mods (romfs only) are not modules

        SysmoduleInfo m;
        m.tid = upper(entry);
        m.autostart = fsx::fileExists(fsx::join(dir, "flags/boot2.flag"));
        std::string text;
        if (fsx::readFile(fsx::join(dir, "toolbox.json"), text)) {
            const auto j = nlohmann::json::parse(text, nullptr, false);
            if (j.is_object()) {
                if (j.contains("name") && j["name"].is_string()) m.name = j["name"].get<std::string>();
                if (j.contains("requires_reboot") && j["requires_reboot"].is_boolean())
                    m.requires_reboot = j["requires_reboot"].get<bool>();
            }
        }
        auto it = owner.find(m.tid);
        if (it != owner.end()) {
            m.package = it->second->name;
            if (m.name.empty()) m.name = it->second->title;
        }
        if (m.name.empty()) m.name = m.tid;
        out.push_back(std::move(m));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const SysmoduleInfo& a, const SysmoduleInfo& b) {
        return fsx::lower(a.name) < fsx::lower(b.name);
    });
    return out;
}

bool setSysmoduleAutostart(const std::string& sd_root, const std::string& tid, bool on, std::string& err) {
    if (!isHexTid(tid)) {
        err = "bad title id";
        return false;
    }
    const std::string dir = contentsDir(sd_root, upper(tid));
    if (!fsx::fileExists(fsx::join(dir, "exefs.nsp"))) {
        err = "no module " + tid;
        return false;
    }
    const std::string flags = fsx::join(dir, "flags");
    const std::string flag = fsx::join(flags, "boot2.flag");
    if (on) {
        fsx::ensureDir(flags);
        FILE* f = std::fopen(flag.c_str(), "wb");
        if (!f) {
            err = "cannot create " + flag;
            return false;
        }
        std::fclose(f);
        return true;
    }
    if (fsx::fileExists(flag) && std::remove(flag.c_str()) != 0) {
        err = "cannot remove " + flag;
        return false;
    }
    fsx::removeDir(flags);  // only if empty
    return true;
}

}  // namespace homebrew
