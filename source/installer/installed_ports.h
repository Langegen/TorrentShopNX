#pragma once

// Ports installed by the auto-installer: one manifest per release in
// TSNX_DATA_DIR/ports/<key>.json with every file written and the forwarders,
// so a port can be shown as installed, updated (files that the new version
// no longer has are removed) and uninstalled.

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace installer {

struct InstalledPort {
    std::string key;            // topic id, or a name derived from the title
    std::string topic_id;
    std::string title;
    std::string variant_id;
    std::string game;
    std::string recipe_id;
    std::time_t installed_at = 0;
    std::vector<std::string> files;          // absolute paths (sdmc:/...)
    std::vector<uint64_t> forwarder_tids;
};

class InstalledPorts {
public:
    static InstalledPorts& instance();

    static std::string keyFor(const std::string& topic_id, const std::string& title);

    bool load(const std::string& key, InstalledPort& out) const;
    bool isInstalled(const std::string& topic_id, const std::string& title) const;

    // Saves the manifest. If the port was installed before, files of the old
    // manifest that the new one does not list are deleted (stale files of the
    // previous version). Returns the number of removed stale files.
    size_t saveReplacing(const InstalledPort& port);

    // Deletes the port's files, the folders left empty, its forwarders and the
    // manifest. `removed_files` counts deleted files.
    bool uninstall(const std::string& key, size_t& removed_files, std::string& out_error);

private:
    std::string manifestPath(const std::string& key) const;
    // Drops paths listed by other installed ports (shared files stay).
    std::vector<std::string> withoutFilesOfOtherPorts(const std::string& key,
                                                      const std::vector<std::string>& files) const;
};

// Deletes `paths` and then their parent folders that became empty (never the
// SD root or sdmc:/switch itself). Returns how many files were deleted.
size_t removeFilesAndEmptyDirs(const std::vector<std::string>& paths);

} // namespace installer
