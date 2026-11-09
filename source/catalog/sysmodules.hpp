#pragma once

// System modules on the SD card: atmosphere/contents/<title id>/exefs.nsp.
// Finding them, their names and the autostart flag (flags/boot2.flag) is file
// work and is tested in pctest; starting and stopping a running module goes
// through the pm services and lives in the UI glue (console only).

#include "homebrew_package.hpp"

#include <map>
#include <string>
#include <vector>

namespace homebrew {

struct SysmoduleInfo {
    std::string tid;             // 16 hex digits, upper case
    std::string name;            // toolbox.json "name", else the store package's title, else the tid
    std::string package;         // store package that owns it ("" if none)
    bool autostart = false;      // flags/boot2.flag present
    bool requires_reboot = false;  // toolbox.json: cannot be started or stopped while the console runs
};

// All modules on the card, sorted by name. pkgs and installed (registry,
// keyed by lower-case package name; either may be null) name the modules that
// come from the store: GitHub packages list their files, fortheusers ones only
// in the registry after install.
std::vector<SysmoduleInfo> scanSysmodules(const std::string& sd_root, const std::vector<Package>* pkgs,
                                          const std::map<std::string, Installed>* installed = nullptr);

// Creates or removes atmosphere/contents/<tid>/flags/boot2.flag.
bool setSysmoduleAutostart(const std::string& sd_root, const std::string& tid, bool on, std::string& err);

// Title ids of the modules a file list installs.
std::vector<std::string> sysmoduleTids(const std::vector<ManifestEntry>& files);

// 16 hex digits -> number (0 if not a title id).
unsigned long long parseTitleId(const std::string& tid);

}  // namespace homebrew
