#pragma once

// Homebrew store data: packages from the Homebrew App Store (fortheusers.org)
// and from our own index (homebrew.json, built by a GitHub Action in
// Langegen/switch-game-collection), merged into one list.
//
// Pure logic, no file or network access, so it is tested on PC
// (pctest/test_homebrew.cpp). Installed packages are tracked the libget way
// (sdmc:/switch/appstore/.get/packages/<name>/manifest.install + info.json),
// so the official Homebrew App Store and we see the same installs.

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace homebrew {

constexpr const char* kFortheusersCdn = "https://switch.cdn.fortheusers.org";
constexpr const char* kRegistryDir = "sdmc:/switch/appstore/.get/packages";

// ---------------------------------------------------------------------------
// Manifest (manifest.install): one "<op>: <path from SD root>" per line.
// Semantics follow libget (fortheusers/libget, Package.cpp):
//   U  written on install and update, deleted on removal
//   E  written on install and update, never deleted
//   G  written only if the file is missing, deleted on removal
//   L  not in the archive (created by the app), deleted on removal
enum class ManifestOp { Update, Extract, Get, Local };

struct ManifestEntry {
    ManifestOp op = ManifestOp::Update;
    std::string path;  // normalized, relative to SD root ("switch/x/x.nro")
};

// Unknown ops and blank lines are skipped. Returns false (with err) when a
// path is unsafe ("..", absolute) or nothing usable was found.
bool parseManifest(const std::string& text, std::vector<ManifestEntry>& out, std::string& err);
std::string formatManifest(const std::vector<ManifestEntry>& entries);

// "sdmc:/switch/a.nro", "/switch/a.nro", "switch\\a.nro" -> "switch/a.nro".
// Returns "" for unsafe paths: "..", "." segments, other drives, empty.
std::string normalizeRelPath(const std::string& in);

// ---------------------------------------------------------------------------
// Where a package may write.
enum class PathVerdict {
    Allowed,    // normal homebrew data
    Warn,       // allowed, but only after manual review on the index side
    Forbidden   // bootloader, firmware, payloads, our own files: package is hidden
};

struct PathCheck {
    PathVerdict verdict = PathVerdict::Allowed;
    std::string reason;  // short English reason for logs ("bootloader")
};

PathCheck checkInstallPath(const std::string& rel_path);

// ---------------------------------------------------------------------------
// What a package contains.
enum Kind : unsigned {
    KindApp       = 1u << 0,  // .nro under switch/
    KindOverlay   = 1u << 1,  // .ovl under switch/.overlays/
    KindSysmodule = 1u << 2   // atmosphere/contents/<tid>/exefs.nsp
};

struct FileAnalysis {
    unsigned kinds = 0;
    std::vector<std::string> sysmodule_tids;  // upper-case hex, 16 chars
    std::vector<std::string> nro_paths;
    PathVerdict worst = PathVerdict::Allowed;
    std::string worst_path;
    std::string worst_reason;
    // One .nro under switch/ or one .ovl under switch/.overlays/ and nothing
    // else: safe to add to the store without review.
    bool simple = false;
};

FileAnalysis analyzeFiles(const std::vector<ManifestEntry>& files);

// ---------------------------------------------------------------------------
// Versions. "v1.2.0" == "1.2.0"; dates "12/02/2025" (dd/mm/yyyy) and
// "2025-02-12" compare as dates. Returns <0, 0, >0. If either side has no
// digits, only equality is known: returns 0 when the strings match, else 1
// (treat as "different, a is newer" - the libget behaviour).
int compareVersions(const std::string& a, const std::string& b);

// "https://github.com/Owner/Repo/releases" -> "owner/repo"; "" if not GitHub.
std::string githubRepoFromUrl(const std::string& url);

// "dd/mm/yyyy" -> "yyyy-mm-dd"; "yyyy-mm-dd..." kept (first 10 chars); else "".
std::string normalizeDate(const std::string& date);
// Whole days from a to b (both "yyyy-mm-dd"); 0 if either is invalid.
int daysBetween(const std::string& a, const std::string& b);

// ---------------------------------------------------------------------------
enum class Source { Fortheusers, Github };
enum class DownloadType { Zip, File };  // File: a single .nro/.ovl written to files[0]

struct Package {
    std::string name;  // registry key (fortheusers name when the package is there)
    std::string title;
    std::string author;
    std::string version;
    std::string category;  // fortheusers categories: tool, game, emu, advanced, theme, ...
    std::string description;
    std::string details;
    std::string changelog;
    std::string license;
    std::string url;       // project page
    std::string repo;      // "owner/repo" when known
    std::string binary;    // main .nro path from fortheusers ("/switch/x/x.nro" or "none")
    std::string updated;   // yyyy-mm-dd
    std::string first_seen;  // yyyy-mm-dd, own index only (auto-found packages)
    bool auto_found = false;

    int64_t download_bytes = 0;
    int64_t extracted_bytes = 0;
    int64_t downloads = 0;  // fortheusers app_dls
    std::string md5;
    std::string sha256;

    Source source = Source::Fortheusers;
    DownloadType download_type = DownloadType::Zip;
    std::string download_url;
    std::string icon_url;
    std::string screen_url;
    std::string manifest_url;  // fortheusers only; own index ships files inline

    // GitHub zips that wrap the SD layout in a folder ("SdOut/"): archive entry
    // = zip_root + file path. Empty for fortheusers zips (entries match paths).
    std::string zip_root;
    std::vector<ManifestEntry> files;  // empty until known (fortheusers: download manifest_url)
    unsigned kinds = 0;                // from files, or from index metadata
    std::vector<std::string> depends;  // package names
    std::map<std::string, std::string> description_i18n;  // "ru" -> short description
};

// Per-package metadata the index adds to fortheusers packages.
struct PackageMeta {
    unsigned kinds = 0;
    bool kinds_known = false;
    std::vector<std::string> depends;
    std::map<std::string, std::string> description_i18n;
};

struct IndexExtras {
    std::set<std::string> hidden;  // lower-case package names or "owner/repo"
    std::vector<std::string> overlay_dependencies;  // installed with the first overlay
    std::map<std::string, PackageMeta> fortheusers_meta;  // key: lower-case name
};

// fortheusers repo.json. Sizes there are in KiB; converted to bytes.
bool parseFortheusersRepo(const std::string& json, std::vector<Package>& out, std::string& err,
                          const std::string& cdn = kFortheusersCdn);

// Our homebrew.json (format: see docs/homebrew-store-plan.md).
bool parseOwnIndex(const std::string& json, std::vector<Package>& out, IndexExtras& extras,
                   std::string& err);

// One list: fortheusers packages enriched by the index, newer GitHub releases
// replace older fortheusers downloads (keeping the fortheusers name so the
// shared registry still matches), packages only in the index are appended,
// hidden ones dropped, overlays get overlay_dependencies. Sorted by title.
std::vector<Package> mergeSources(const std::vector<Package>& fortheusers,
                                  const std::vector<Package>& own, const IndexExtras& extras);

// Shown with the "New" badge: auto-found and first seen within `days`.
bool isNew(const Package& pkg, const std::string& today, int days = 14);

// ---------------------------------------------------------------------------
// Installed state (libget registry).
struct Installed {
    bool present = false;          // manifest.install exists
    std::string version;           // from info.json, "" if missing
    std::vector<ManifestEntry> manifest;
};

enum class Status { NotInstalled, Installed, UpdateAvailable };

Status statusFor(const Package& pkg, const Installed& inst);

// info.json in the registry: {"version": "..."} (what libget reads).
std::string parseInfoVersion(const std::string& json);
std::string makeInfoJson(const Package& pkg);

struct InstallPlan {
    std::vector<ManifestEntry> write;       // extract these from the archive
    std::vector<std::string> keep_existing; // G entries already on the card
    std::vector<std::string> remove_stale;  // U entries of the old version gone from the new one
};

// exists(rel_path) tells whether a file is on the card.
InstallPlan planInstall(const std::vector<ManifestEntry>& new_manifest,
                        const std::vector<ManifestEntry>& old_manifest,
                        const std::function<bool(const std::string&)>& exists);

struct RemovalPlan {
    std::vector<std::string> remove_files;
    std::vector<std::string> remove_dirs;  // deepest first; rmdir only (fails if not empty)
    std::vector<std::string> kept;         // E entries, and settings when keep_settings
};

// keep_settings: G entries and anything under config/ stay.
RemovalPlan planRemoval(const std::vector<ManifestEntry>& manifest, bool keep_settings);

}  // namespace homebrew
