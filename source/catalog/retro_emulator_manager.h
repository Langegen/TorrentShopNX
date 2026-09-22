#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <atomic>
#include <nanovg.h>

namespace catalog {

enum class EmulatorInstallStatus {
    NOT_INSTALLED,
    INSTALLED,
    UPDATE_AVAILABLE
};

struct EmulatorPackage {
    std::string id;
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    std::string category; // "nintendo", "sony", "sega", "retroarch"
    std::string download_url;
    std::string filename;
    std::string install_path; // e.g. "sdmc:/switch/dekopon/dekopon.nro"
    std::string extract_dir;  // if archive, directory to extract into
    bool is_archive = false;
    int64_t file_size = 0;
    std::vector<std::string> supported_console_ids;
    NVGcolor color;
};

class RetroEmulatorManager {
public:
    static RetroEmulatorManager& instance();

    const std::vector<EmulatorPackage>& getPackages() const { return packages_; }
    const EmulatorPackage* findPackage(const std::string& emu_id) const;
    const EmulatorPackage* getPackageForConsole(const std::string& console_id) const;

    EmulatorInstallStatus getInstallStatus(const std::string& emu_id) const;
    bool isInstalled(const std::string& emu_id) const;
    std::string getInstalledVersion(const std::string& emu_id) const;

    void recordInstalledVersion(const std::string& emu_id, const std::string& version);
    bool uninstallEmulator(const std::string& emu_id, std::string& out_err);

    std::string getManifestDownloadUrl() const;
    std::string getLocalManifestPath() const;
    bool loadLocalManifest();
    bool refreshManifest(std::function<void(float progress, const std::string& status)> progress_cb = nullptr);
    bool parseManifestFromJson(const std::string& json_str);

    // Resolve relative path according to platform (Switch vs PC)
    static std::string resolvePlatformPath(const std::string& path);

    void healInstalledEmulators();

private:
    RetroEmulatorManager();
    void initPackages();
    void loadInstalledVersions();
    void saveInstalledVersions();

    std::vector<EmulatorPackage> packages_;
    std::unordered_map<std::string, std::string> installed_versions_;
};

} // namespace catalog
