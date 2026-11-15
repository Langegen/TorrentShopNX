#include "homebrew_installer.hpp"

#include "../utils/atomic_file.h"
#include "../utils/hash_utils.h"
#include "../utils/zip_reader.h"
#include "homebrew_fs.hpp"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <functional>
#include <set>
#include <sstream>
#include <utility>

namespace homebrew {

namespace {

using fsx::ensureDir;
using fsx::fileExists;
using fsx::join;
using fsx::lower;
using fsx::parentOf;
using fsx::readFile;
using fsx::removeDir;

std::string safeName(const std::string& s) {
    std::string out;
    for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.') ? c : '_';
    return out.empty() ? "pkg" : out;
}

// Like parseManifest, but a bad line (a tampered registry) only drops that line.
std::vector<ManifestEntry> parseManifestLenient(const std::string& text) {
    std::vector<ManifestEntry> out;
    std::istringstream is(text);
    std::string line, err;
    while (std::getline(is, line)) {
        std::vector<ManifestEntry> one;
        if (parseManifest(line, one, err)) out.insert(out.end(), one.begin(), one.end());
    }
    return out;
}

bool copyFile(const std::string& from, const std::string& to) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(to.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }
    std::vector<char> buf(256 * 1024);
    bool ok = true;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0)
        if (std::fwrite(buf.data(), 1, n, out) != n) {
            ok = false;
            break;
        }
    if (std::ferror(in)) ok = false;
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    if (!ok) std::remove(to.c_str());
    return ok;
}

}  // namespace

const char* installErrorName(InstallError e) {
    switch (e) {
        case InstallError::None: return "none";
        case InstallError::Cancelled: return "cancelled";
        case InstallError::Network: return "network";
        case InstallError::Checksum: return "checksum";
        case InstallError::NoSpace: return "no_space";
        case InstallError::Forbidden: return "forbidden";
        case InstallError::BadArchive: return "bad_archive";
        case InstallError::Write: return "write";
        case InstallError::NotInstalled: return "not_installed";
    }
    return "?";
}

Installer::Installer(InstallerConfig cfg, Fetcher fetch, DownloadFn download, FreeSpaceFn free_space)
    : cfg_(std::move(cfg)), fetch_(std::move(fetch)), download_(std::move(download)),
      free_space_(std::move(free_space)) {}

std::string Installer::registryDirFor(const std::string& name) const {
    const std::string key = lower(name);
    std::string found;
    if (DIR* d = opendir(cfg_.registry_dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != ".." && lower(n) == key && fsx::dirExists(join(cfg_.registry_dir, n))) {
                found = join(cfg_.registry_dir, n);
                break;
            }
        }
        closedir(d);
    }
    return found;
}

InstallResult Installer::install(const Package& pkg, const Installed& current, const InstallProgressFn& progress,
                                 const std::shared_ptr<std::atomic<bool>>& cancel) {
    std::lock_guard<std::mutex> guard(mutex_);
    InstallResult res;
    std::string dl;  // downloaded file, removed on every exit
    auto fail = [&](InstallError e, const std::string& msg) {
        if (!dl.empty()) std::remove(dl.c_str());
        res.ok = false;
        res.error = e;
        res.message = msg;
        return res;
    };
    auto cancelled = [&] { return cancel && cancel->load(); };
    auto report = [&](InstallProgress::Stage st, float frac, int64_t done = 0, int64_t total = 0,
                      const std::string& file = "") {
        if (!progress) return;
        InstallProgress p;
        p.stage = st;
        p.fraction = std::min(1.f, std::max(0.f, frac));
        p.done = done;
        p.total = total;
        p.file = file;
        progress(p);
    };

    // 1. What goes where.
    report(InstallProgress::Stage::Prepare, 0.f);
    std::vector<ManifestEntry> files = pkg.files;
    if (files.empty()) {
        if (pkg.manifest_url.empty()) return fail(InstallError::BadArchive, "no file list");
        const FetchResult r = fetch_ ? fetch_(pkg.manifest_url, "") : FetchResult{};
        if (r.status != 200) return fail(InstallError::Network, "manifest: HTTP " + std::to_string(r.status));
        std::string err;
        if (!parseManifest(r.body, files, err)) {
            // An unsafe path in a manifest is the same as a forbidden one.
            return fail(err.find("unsafe") != std::string::npos ? InstallError::Forbidden : InstallError::BadArchive,
                        "manifest: " + err);
        }
    }
    if (pkg.download_type == DownloadType::File && files.size() != 1)
        return fail(InstallError::BadArchive, "single-file package with " + std::to_string(files.size()) + " files");
    const FileAnalysis fa = analyzeFiles(files);
    if (fa.worst == PathVerdict::Forbidden)
        return fail(InstallError::Forbidden, fa.worst_path + " (" + fa.worst_reason + ")");

    // 2. Room for the download and the unpacked files (the old version stays until the end).
    const int64_t unpacked = pkg.extracted_bytes > 0 ? pkg.extracted_bytes : pkg.download_bytes * 2;
    const int64_t need = unpacked + pkg.download_bytes + 1024 * 1024;
    const int64_t free_bytes = free_space_ ? free_space_(cfg_.sd_root) : -1;
    if (free_bytes >= 0 && free_bytes < need)
        return fail(InstallError::NoSpace, "need " + std::to_string(need) + ", free " + std::to_string(free_bytes));
    if (cancelled()) return fail(InstallError::Cancelled, "cancelled");

    // 3. Download.
    ensureDir(cfg_.tmp_dir);
    dl = join(cfg_.tmp_dir, "hb_" + safeName(pkg.name) + (pkg.download_type == DownloadType::Zip ? ".zip" : ".bin"));
    std::remove(dl.c_str());
    {
        std::string err;
        const bool ok = download_ && download_(pkg.download_url, dl,
                                               [&](int64_t done, int64_t total) {
                                                   const float f = total > 0 ? float(done) / float(total) : 0.f;
                                                   report(InstallProgress::Stage::Download, 0.05f + 0.55f * f, done, total);
                                               },
                                               cancel.get(), err);
        if (cancelled()) return fail(InstallError::Cancelled, "cancelled");
        if (!ok || !fileExists(dl)) return fail(InstallError::Network, "download: " + err);
    }

    // 4. Checksum.
    report(InstallProgress::Stage::Verify, 0.6f);
    if (!pkg.sha256.empty() || !pkg.md5.empty()) {
        std::string sha, md5;
        if (!util::hashFile(dl, pkg.sha256.empty() ? nullptr : &sha, pkg.sha256.empty() ? &md5 : nullptr))
            return fail(InstallError::Write, "cannot read the download");
        if (!pkg.sha256.empty() ? sha != lower(pkg.sha256) : md5 != lower(pkg.md5))
            return fail(InstallError::Checksum, pkg.sha256.empty() ? "md5 " + md5 : "sha256 " + sha);
    }
    if (cancelled()) return fail(InstallError::Cancelled, "cancelled");

    // 5. Every file goes next to its target as <file>.hbnew first.
    const std::vector<ManifestEntry> old_files = current.untracked ? std::vector<ManifestEntry>{} : current.manifest;
    const InstallPlan plan = planInstall(files, old_files,
                                         [&](const std::string& p) { return fileExists(cfg_.sd_root + p); });
    res.kept = plan.keep_existing;

    std::vector<std::string> created_dirs;
    std::vector<std::pair<std::string, std::string>> staged;  // target, tmp
    util::ZipReader zip;
    auto rollback = [&] {
        zip.close();  // an open file cannot be deleted (SD card, Windows): fail() removes the download next
        for (const auto& s : staged) std::remove(s.second.c_str());
        for (auto it = created_dirs.rbegin(); it != created_dirs.rend(); ++it) removeDir(*it);
        staged.clear();
    };

    int64_t total_bytes = 0;
    if (pkg.download_type == DownloadType::Zip) {
        std::string err;
        if (!zip.open(dl, err)) {
            zip.close();
            return fail(InstallError::BadArchive, err);
        }
        for (const auto& w : plan.write)
            if (const util::ZipEntry* ze = zip.find(pkg.zip_root + w.path)) total_bytes += static_cast<int64_t>(ze->usize);
    } else {
        struct stat st;
        if (stat(dl.c_str(), &st) == 0) total_bytes = st.st_size;
    }

    int64_t done_bytes = 0;
    for (const auto& w : plan.write) {
        if (cancelled()) {
            rollback();
            return fail(InstallError::Cancelled, "cancelled");
        }
        const std::string target = cfg_.sd_root + w.path;
        const std::string tmp = target + ".hbnew";
        ensureDir(parentOf(target), [&](const std::string& d) { created_dirs.push_back(d); });
        auto extracting = [&](uint64_t n) {
            const float f = total_bytes > 0 ? float(done_bytes + int64_t(n)) / float(total_bytes) : 0.f;
            report(InstallProgress::Stage::Extract, 0.65f + 0.3f * f, done_bytes + int64_t(n), total_bytes, w.path);
        };
        if (pkg.download_type == DownloadType::File) {
            if (!copyFile(dl, tmp)) {
                rollback();
                return fail(InstallError::Write, "cannot write " + w.path);
            }
            done_bytes = total_bytes;
            extracting(0);
        } else {
            const util::ZipEntry* ze = zip.find(pkg.zip_root + w.path);
            if (!ze || ze->is_dir) {
                res.missing.push_back(w.path);
                continue;
            }
            std::string err;
            if (!zip.extract(*ze, tmp, cancel.get(), extracting, err)) {
                rollback();
                if (cancelled()) return fail(InstallError::Cancelled, "cancelled");
                const bool write_err = err.rfind("cannot", 0) == 0;
                return fail(write_err ? InstallError::Write : InstallError::BadArchive, err);
            }
            done_bytes += static_cast<int64_t>(ze->usize);
        }
        staged.emplace_back(target, tmp);
    }
    zip.close();
    if (staged.empty() && !plan.write.empty()) {
        rollback();
        return fail(InstallError::BadArchive, "none of the listed files is in the archive");
    }
    if (cancelled()) {
        rollback();
        return fail(InstallError::Cancelled, "cancelled");
    }

    // 6. Swap in: old files aside, new ones in place; any failure puts the old ones back.
    report(InstallProgress::Stage::Finish, 0.95f);
    std::vector<std::string> moved_aside, placed;
    auto undo = [&] {
        for (const auto& t : placed) std::remove(t.c_str());
        for (const auto& t : moved_aside) std::rename((t + ".hbold").c_str(), t.c_str());
        rollback();
    };
    for (const auto& s : staged) {
        if (!fileExists(s.first)) continue;
        std::remove((s.first + ".hbold").c_str());
        if (std::rename(s.first.c_str(), (s.first + ".hbold").c_str()) != 0) {
            undo();
            return fail(InstallError::Write, "cannot replace " + s.first);
        }
        moved_aside.push_back(s.first);
    }
    for (const auto& s : staged) {
        if (std::rename(s.second.c_str(), s.first.c_str()) != 0) {
            undo();
            return fail(InstallError::Write, "cannot place " + s.first);
        }
        placed.push_back(s.first);
    }
    for (const auto& t : moved_aside) std::remove((t + ".hbold").c_str());
    for (const auto& s : staged) res.written.push_back(s.first.substr(cfg_.sd_root.size()));

    // 7. Files of the old version that the new one no longer has.
    std::vector<ManifestEntry> stale;
    for (const auto& p : plan.remove_stale) {
        if (checkInstallPath(p).verdict == PathVerdict::Forbidden) continue;
        if (std::remove((cfg_.sd_root + p).c_str()) == 0) res.removed.push_back(p);
        stale.push_back(ManifestEntry{ManifestOp::Update, p});
    }
    for (const auto& d : planRemoval(stale, false).remove_dirs) removeDir(cfg_.sd_root + d);

    // 8. Registry (libget layout), in the existing folder whatever its case.
    std::string reg = registryDirFor(pkg.name);
    if (reg.empty()) reg = join(cfg_.registry_dir, pkg.name);
    ensureDir(reg);
    if (!util::atomicWriteFile(join(reg, "manifest.install"), formatManifest(files)) ||
        !util::atomicWriteFile(join(reg, "info.json"), makeInfoJson(pkg)))
        return fail(InstallError::Write, "files installed, registry not written");

    std::remove(dl.c_str());
    report(InstallProgress::Stage::Finish, 1.f);
    res.ok = true;
    return res;
}

InstallResult Installer::uninstall(const std::string& name, bool keep_settings, const InstallProgressFn& progress) {
    std::lock_guard<std::mutex> guard(mutex_);
    InstallResult res;
    const std::string reg = registryDirFor(name);
    std::string text;
    if (reg.empty() || !readFile(join(reg, "manifest.install"), text)) {
        res.error = InstallError::NotInstalled;
        res.message = "no registry entry for " + name;
        return res;
    }
    const RemovalPlan plan = planRemoval(parseManifestLenient(text), keep_settings);
    res.kept = plan.kept;
    size_t i = 0;
    for (const auto& f : plan.remove_files) {
        ++i;
        if (progress) {
            InstallProgress p;
            p.stage = InstallProgress::Stage::Remove;
            p.file = f;
            p.fraction = float(i) / float(plan.remove_files.size());
            progress(p);
        }
        if (checkInstallPath(f).verdict == PathVerdict::Forbidden) {
            res.kept.push_back(f);  // never delete outside what a package may own
            continue;
        }
        if (std::remove((cfg_.sd_root + f).c_str()) == 0) res.removed.push_back(f);
    }
    for (const auto& d : plan.remove_dirs) removeDir(cfg_.sd_root + d);  // only empty ones go

    for (const char* f : {"manifest.install", "info.json", "icon.png"}) std::remove(join(reg, f).c_str());
    removeDir(reg);
    res.ok = true;
    return res;
}

std::vector<Package> missingDependencies(const Package& pkg, const Catalog& catalog) {
    std::vector<Package> out;
    std::set<std::string> seen{lower(pkg.name)};
    std::function<void(const Package&)> visit = [&](const Package& p) {
        for (const auto& dep : p.depends) {
            if (!seen.insert(lower(dep)).second) continue;
            Package d;
            if (!catalog.find(dep, d)) continue;
            if (catalog.status(d) != Status::NotInstalled) continue;
            visit(d);
            out.push_back(d);
        }
    };
    visit(pkg);
    return out;
}

}  // namespace homebrew
