#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include "torrent/torrent_manager.h"
#include "utils/archive_peek.h"

namespace installer {

struct PortVariant {
    std::string id;            // "ru", "en", "modded", "vanilla", "main", "v1"..., "all", "default"
    std::string title;         // Fallback name (e.g. "Русская версия (RUS)", a folder name, a recipe title)
    std::string title_key;     // i18n key for the UI ("app/ports/variant_ru"); empty: use title
    std::string title_arg;     // {0} of title_key (folder name of a version)
    std::vector<int> file_indices;            // variant-only files (archives, loose files, own forwarders)
    std::vector<int> forwarder_nsp_indices;   // NSP shortcuts of this variant (empty: use the common one)
    int64_t total_size = 0;
    std::vector<std::string> check_paths;     // must exist after install (recipe "check")
};

struct PortArchiveTask {
    int file_index = -1;               // first volume
    std::vector<int> volume_indices;   // all volumes in order (one entry for a plain archive)
    std::string archive_torrent_path;
    std::string extract_dest_dir;      // Usually "sdmc:/"
    int64_t compressed_size = 0;       // sum of all volumes
    std::string unsupported_reason;    // non-empty: the archive cannot be streamed (e.g. multi-volume RAR)

    // Filled from the archive's own listing (refineArchiveDestination) or a recipe
    std::string strip_prefix;          // removed from entry paths ("Homebrew/", "Wrapper/")
    std::string only_subpath;          // extract only entries under it (recipe "subpath")
    uint64_t uncompressed_size = 0;    // 0: unknown
    std::string dest_reason;           // why extract_dest_dir was chosen (for logs)
    std::string top_folder;            // the archive's only top folder (after strip_prefix), if any
    bool from_recipe = false;          // destination fixed by a recipe: never guessed
};

struct PortInstallPlan {
    bool is_port = false;
    std::string port_game_name; // e.g. "codmw", "masseffect-nx", "nfsu2x", "Simpsons", etc.

    // Common NSP forwarder (used by variants that have no forwarder of their own)
    int forwarder_nsp_index = -1;
    std::string forwarder_nsp_name;
    int64_t forwarder_nsp_size = 0;

    // Variants (languages, mods, separate versions). Always at least one for a port.
    std::vector<PortVariant> variants;
    int default_variant_index = 0;

    // Files that apply regardless of chosen variant
    std::vector<int> common_file_indices;

    // Mapping: torrent file index -> target path on SD card (e.g. "sdmc:/switch/codmw/game/...").
    // NSP forwarders map to cache/tmp; archive volumes map to their extraction dir.
    std::unordered_map<int, std::string> file_destinations;

    // Archives to extract (install order: archives first, then loose files on top of them)
    std::vector<PortArchiveTask> archives_to_extract;

    // Files that are not installed, with the reason (readme/screenshots, unsafe paths, big NSPs)
    std::unordered_map<int, std::string> skipped_files;

    std::string recipe_id;   // non-empty: built from a recipe (topic id)
    std::string note;        // recipe note shown after install (e.g. how to launch)

    // All file indices needed for a variant (common + variant files, incl. its forwarder)
    std::vector<int> getFilesForVariant(int variant_idx) const;

    // Archives needed for a variant, in install order (common first, then the variant's own)
    std::vector<PortArchiveTask> getArchivesForVariant(int variant_idx) const;

    // Loose (copied) files of a variant in install order: common first, then the
    // variant's own, so variant files overwrite common ones. No archives, no NSPs.
    std::vector<int> getLooseFilesForVariant(int variant_idx) const;

    // Forwarder NSPs for a variant (its own, else the common one; empty if none)
    std::vector<int> forwardersForVariant(int variant_idx) const;

    // Total download size for a variant
    int64_t getDownloadSizeForVariant(int variant_idx, const std::vector<torrent::TorrentFileInfo>& all_files) const;

    // Human-readable dump of the plan (one line per file) for logs and tests
    std::string describe(const std::vector<torrent::TorrentFileInfo>& all_files, size_t max_files = 300) const;
};

// True if writing to this path (relative to the SD root, "/" separators) could
// break the console or the CFW: bootloader/, Nintendo/, emuMMC/, atmosphere/
// outside contents/, hbmenu.nro, boot.dat, payload.bin...
bool isProtectedSdPath(const std::string& relative_to_sd_root);

// Picks where an archive goes from its listing (7z header / ZIP central
// directory): switch/ or atmosphere/ inside -> SD root; one folder with an
// .nro -> sdmc:/switch/; loose game files -> sdmc:/switch/<game>/; a folder the
// release already uses elsewhere follows it. Unwraps a meaningless top folder
// and a "Homebrew"/"sdcard" folder. Updates task.extract_dest_dir,
// strip_prefix, uncompressed_size and dest_reason. Recipe tasks are not changed.
void refineArchiveDestination(const PortInstallPlan& plan,
                              PortArchiveTask& task,
                              const std::vector<util::ArchiveEntryInfo>& entries);

// Where one archive entry is written, relative to task.extract_dest_dir;
// "" = skip (outside only_subpath, or a protected system path).
std::string archiveEntryTarget(const PortArchiveTask& task, const std::string& entry_path);

// ── Recipes: exact install steps for releases the heuristics get wrong ─────
//
// JSON (resources/ports/recipes.json, overridden by TSNX_DATA_DIR/port_recipes.json):
// { "version": 1, "recipes": [ {
//     "topic_id": "6012345", "title_match": "Max Payne Mobile", "game": "maxpayne",
//     "default": "nx", "combinable": true, "note": "...",
//     "variants": [ { "id": "nx", "title": "...", "steps": [
//         { "extract": "maxpaynenx.7z", "to": "sdmc:/" },
//         { "extract": "PCAE Switch/PCAE Switch.zip", "subpath": "maxpayne/", "to": "sdmc:/switch/" },
//         { "copy": "Homebrew/maxpaynenx/", "to": "sdmc:/maxpaynenx/" },
//         { "install_nsp": "PCAE Switch/MaxPayne.nsp" } ],
//       "check": [ "sdmc:/switch/maxpayne/maxpayne.nro" ] } ] } ] }
// Paths are relative to the torrent root (its own top folder removed); a
// trailing "/" means a folder. "subpath" extracts one folder of the archive
// (the folder itself is kept).

struct PortRecipeStep {
    enum class Kind { Extract, Copy, InstallNsp };
    Kind kind = Kind::Copy;
    std::string from;
    std::string to;
    std::string subpath;
};

struct PortRecipeVariant {
    std::string id;
    std::string title;
    std::vector<PortRecipeStep> steps;
    std::vector<std::string> check;
};

struct PortRecipe {
    std::string topic_id;
    std::string title_match;     // case-insensitive substring of the release title
    std::string game;
    std::string default_variant;
    bool combinable = false;
    std::string note;
    std::vector<PortRecipeVariant> variants;
};

bool parsePortRecipes(const std::string& json_text, std::vector<PortRecipe>& out, std::string& out_error);

// Recipe by topic id, else by title_match.
const PortRecipe* findPortRecipe(const std::vector<PortRecipe>& recipes,
                                 const std::string& topic_id,
                                 const std::string& release_title);

// Recipes from romfs, then the downloaded file (port_recipes_online.json,
// refreshed with the catalog), then the user's own data/port_recipes.json;
// a later file overrides recipes for the same release. Thread-safe.
class PortRecipeStore {
public:
    static PortRecipeStore& instance();
    bool find(const std::string& topic_id, const std::string& release_title, PortRecipe& out);
    void reload();
private:
    std::mutex mtx_;
    bool loaded_ = false;
    std::vector<PortRecipe> recipes_;
};

// Downloaded recipes (written by the catalog updater).
std::string onlinePortRecipesPath();

class PortInstallPlanner {
public:
    static PortInstallPlanner& instance();

    // Analyzes torrent file list and constructs an execution plan
    PortInstallPlan analyze(const std::string& release_title,
                            const std::vector<torrent::TorrentFileInfo>& files,
                            const std::string& preferred_lang = "ru");

    // Builds the plan from a recipe; false (and out_error) if the release does
    // not match it (a file of a step is missing, a target is protected...).
    bool analyzeWithRecipe(const PortRecipe& recipe,
                           const std::string& release_title,
                           const std::vector<torrent::TorrentFileInfo>& files,
                           const std::string& preferred_lang,
                           PortInstallPlan& out,
                           std::string& out_error);

    // Recipe if there is one that fits, otherwise heuristics.
    PortInstallPlan plan(const std::string& topic_id,
                         const std::string& release_title,
                         const std::vector<torrent::TorrentFileInfo>& files,
                         const std::string& preferred_lang);
};

} // namespace installer
