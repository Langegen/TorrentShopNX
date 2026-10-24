// Table test for installer::PortInstallPlanner: torrent file list -> install plan.
//
// Each pctest/port_fixtures/*.txt holds one release:
//   # title: <release title>
//   # lang: ru                      (UI language, default "ru")
//   # topic: 6012345                (plan through recipes, resources/ports/recipes.json)
//   # use_recipes: yes              (same, matching recipes by title)
//   <size><TAB><path>               (one line per torrent file, in torrent order;
//                                    spaces instead of the TAB work too)
//   ## expect
//   game <folder>                   sdmc:/switch/<folder>
//   variants <id>,<id>,...
//   default <id>                    default variant for "# lang"
//   default_en <id>                 default variant for an English UI
//   dest <path> => <sdmc path>      file is copied there
//   extract <path> => <dir>         archive (first volume) is extracted there
//   volumes <path> = <n>
//   unsupported <path>              archive cannot be streamed
//   skip <path>                     file is not installed
//   in <variant> <path>             file is downloaded for that variant
//   notin <variant> <path>
//   forwarder <variant> <path|->    NSP installed for that variant ("-": none)
//   listing <archive> = a/b.nro, a/c/, ...   archive contents (applied first; "/" = folder)
//   entry <archive> | <entry> => <target|SKIP>   where an archive entry is written
//   recipe <id|->                   plan was built from that recipe ("-": heuristics)
//   check <variant> <sdmc path>     recipe check path of the variant
// <path> matches the end of a torrent path ("/" separators).
//
// File lists can be taken from the console log: the installer writes every file
// as "port_file\t<size>\t<path>", so `grep port_file log.txt | cut -f2-` gives
// the list part of a fixture.
//
// Build and run from the repo root:  powershell -File pctest\build_port_planner.ps1

#include "../source/installer/PortInstallPlanner.hpp"
#include "../source/datasource/concat_data_source.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

std::atomic<bool> g_appExiting{false};

namespace util {
void logLine(const std::string&) {}
// 7z listing needs the 7-Zip SDK; not linked into this test.
bool peek7zArchiveFromSource(datasource::IDataSource*, uint64_t, const std::shared_ptr<std::atomic<bool>>&,
                             ArchivePeek&, std::string& err) {
    err = "not linked";
    return false;
}
}

namespace {

struct Fixture {
    std::string file;
    std::string title;
    std::string lang = "ru";
    std::string topic;
    bool useRecipes = false;
    std::vector<torrent::TorrentFileInfo> files;
    std::vector<std::pair<int, std::string>> expects;   // line number, text
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool loadFixture(const std::filesystem::path& p, Fixture& fx) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    fx.file = p.filename().string();
    std::string line;
    bool expect = false;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lineNo == 1 && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line = line.substr(3);
        std::string t = trim(line);
        if (t.empty()) continue;
        if (t == "## expect") { expect = true; continue; }
        if (expect) {
            if (t[0] != '#') fx.expects.push_back({lineNo, t});
            continue;
        }
        if (t.rfind("# title:", 0) == 0) { fx.title = trim(t.substr(8)); continue; }
        if (t.rfind("# lang:", 0) == 0) { fx.lang = trim(t.substr(7)); continue; }
        if (t.rfind("# topic:", 0) == 0) { fx.topic = trim(t.substr(8)); fx.useRecipes = true; continue; }
        if (t.rfind("# use_recipes:", 0) == 0) { fx.useRecipes = trim(t.substr(14)) == "yes"; continue; }
        if (t[0] == '#') continue;
        // <size><TAB or spaces><path>
        size_t digits = 0;
        while (digits < t.size() && std::isdigit(static_cast<unsigned char>(t[digits]))) ++digits;
        size_t pathStart = t.find_first_not_of(" \t", digits);
        if (digits == 0 || pathStart == std::string::npos || pathStart == digits) {
            std::cerr << fx.file << ":" << lineNo << ": expected <size><TAB><path>\n";
            return false;
        }
        torrent::TorrentFileInfo f;
        f.index = static_cast<int>(fx.files.size());
        f.size = std::stoull(t.substr(0, digits));
        f.name = t.substr(pathStart);
        fx.files.push_back(f);
    }
    return true;
}

bool pathMatches(const std::string& full, const std::string& tail) {
    if (full.size() < tail.size()) return false;
    if (full.compare(full.size() - tail.size(), tail.size(), tail) != 0) return false;
    return full.size() == tail.size() || full[full.size() - tail.size() - 1] == '/';
}

class Checker {
public:
    Checker(const Fixture& fx) : fx_(fx) { plan_ = makePlan(fx.lang); }

    installer::PortInstallPlan makePlan(const std::string& lang) const {
        auto& p = installer::PortInstallPlanner::instance();
        return fx_.useRecipes ? p.plan(fx_.topic, fx_.title, fx_.files, lang)
                              : p.analyze(fx_.title, fx_.files, lang);
    }

    bool run() {
        // "listing" lines describe archive contents; apply them before the checks.
        // Two passes, like the installer: the second one sees where the other archives went.
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& e : fx_.expects) {
                if (e.second.rfind("listing ", 0) == 0) applyListing(e.first, e.second.substr(8));
            }
        }
        for (const auto& e : fx_.expects) {
            if (e.second.rfind("listing ", 0) != 0) check(e.first, e.second);
        }
        if (failures_ > 0) {
            std::cerr << plan_.describe(fx_.files) << "\n";
        }
        return failures_ == 0;
    }

private:
    void fail(int line, const std::string& what) {
        ++failures_;
        std::cerr << "  FAIL " << fx_.file << ":" << line << ": " << what << "\n";
    }

    int fileIndex(int line, const std::string& tail) {
        int found = -1;
        for (const auto& f : fx_.files) {
            if (pathMatches(f.name, tail)) {
                if (found >= 0) {
                    fail(line, "path '" + tail + "' matches more than one file");
                    return -1;
                }
                found = f.index;
            }
        }
        if (found < 0) fail(line, "no file matches '" + tail + "'");
        return found;
    }

    int variantIndex(int line, const std::string& id) {
        for (size_t i = 0; i < plan_.variants.size(); ++i) {
            if (plan_.variants[i].id == id) return static_cast<int>(i);
        }
        fail(line, "no variant '" + id + "'");
        return -1;
    }

    // "a => b" -> {a, b}
    static bool splitArrow(const std::string& s, std::string& a, std::string& b) {
        size_t p = s.find(" => ");
        if (p == std::string::npos) return false;
        a = trim(s.substr(0, p));
        b = trim(s.substr(p + 4));
        return true;
    }

    installer::PortArchiveTask* taskFor(int line, const std::string& path) {
        int idx = fileIndex(line, path);
        if (idx < 0) return nullptr;
        for (auto& a : plan_.archives_to_extract) {
            if (a.file_index == idx) return &a;
        }
        fail(line, path + " is not an archive task");
        return nullptr;
    }

    // listing <archive> = entry, entry/, ...   ("/" at the end: folder)
    void applyListing(int line, const std::string& arg) {
        size_t eq = arg.find(" = ");
        if (eq == std::string::npos) return fail(line, "syntax: listing <archive> = a, b/, ...");
        auto* task = taskFor(line, trim(arg.substr(0, eq)));
        if (!task) return;
        std::vector<util::ArchiveEntryInfo> entries;
        std::stringstream ss(arg.substr(eq + 3));
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = trim(item);
            if (item.empty()) continue;
            util::ArchiveEntryInfo e;
            e.is_dir = item.back() == '/';
            while (!item.empty() && item.back() == '/') item.pop_back();
            e.path = item;
            e.size = e.is_dir ? 0 : 1000;
            entries.push_back(e);
        }
        installer::refineArchiveDestination(plan_, *task, entries);
        // the same task copy lives in variants' archive lists via file_index; nothing else to sync
    }

    void check(int line, const std::string& text) {
        size_t sp = text.find(' ');
        std::string cmd = text.substr(0, sp);
        std::string arg = sp == std::string::npos ? "" : trim(text.substr(sp + 1));

        if (cmd == "entry") {
            // entry <archive> | <entry path> => <target or SKIP>
            size_t bar = arg.find(" | ");
            std::string rest, target;
            if (bar == std::string::npos || !splitArrow(arg.substr(bar + 3), rest, target))
                return fail(line, "syntax: entry <archive> | <entry> => <target|SKIP>");
            auto* task = taskFor(line, trim(arg.substr(0, bar)));
            if (!task) return;
            std::string got = installer::archiveEntryTarget(*task, rest);
            if (got.empty()) got = "SKIP";
            if (got != target) fail(line, "entry " + rest + " -> '" + got + "' != '" + target + "'");
            return;
        }
        if (cmd == "recipe") {
            std::string got = plan_.recipe_id.empty() ? "-" : plan_.recipe_id;
            if (got != arg) fail(line, "recipe '" + got + "' != '" + arg + "'");
            return;
        }
        if (cmd == "check") {
            // check <variant> <sdmc path>
            size_t s2 = arg.find(' ');
            if (s2 == std::string::npos) return fail(line, "syntax: check <variant> <path>");
            int v = variantIndex(line, arg.substr(0, s2));
            if (v < 0) return;
            const auto& cp = plan_.variants[v].check_paths;
            if (std::find(cp.begin(), cp.end(), trim(arg.substr(s2 + 1))) == cp.end()) fail(line, "no check path " + arg);
            return;
        }
        if (cmd == "game") {
            if (plan_.port_game_name != arg) fail(line, "game '" + plan_.port_game_name + "' != '" + arg + "'");
        } else if (cmd == "variants") {
            std::string got;
            for (const auto& v : plan_.variants) got += (got.empty() ? "" : ",") + v.id;
            if (got != arg) fail(line, "variants '" + got + "' != '" + arg + "'");
        } else if (cmd == "default" || cmd == "default_en") {
            installer::PortInstallPlan p = cmd == "default" ? plan_ : makePlan("en");
            std::string got = p.variants.empty() ? "" : p.variants[p.default_variant_index].id;
            if (got != arg) fail(line, cmd + " '" + got + "' != '" + arg + "'");
        } else if (cmd == "dest") {
            std::string path, dest;
            if (!splitArrow(arg, path, dest)) return fail(line, "syntax: dest <path> => <dest>");
            int idx = fileIndex(line, path);
            if (idx < 0) return;
            if (plan_.skipped_files.count(idx)) return fail(line, path + " skipped: " + plan_.skipped_files.at(idx));
            auto it = plan_.file_destinations.find(idx);
            std::string got = it == plan_.file_destinations.end() ? "(none)" : it->second;
            if (got != dest) fail(line, path + " -> '" + got + "' != '" + dest + "'");
            for (const auto& a : plan_.archives_to_extract) {
                for (int v : a.volume_indices) {
                    if (v == idx) fail(line, path + " is extracted, not copied");
                }
            }
        } else if (cmd == "extract") {
            std::string path, dir;
            if (!splitArrow(arg, path, dir)) return fail(line, "syntax: extract <path> => <dir>");
            int idx = fileIndex(line, path);
            if (idx < 0) return;
            for (const auto& a : plan_.archives_to_extract) {
                if (a.file_index == idx) {
                    if (a.extract_dest_dir != dir) fail(line, path + " extracts to '" + a.extract_dest_dir + "' != '" + dir + "'");
                    return;
                }
            }
            fail(line, path + " is not an archive task");
        } else if (cmd == "volumes") {
            size_t eq = arg.rfind(" = ");
            if (eq == std::string::npos) return fail(line, "syntax: volumes <path> = <n>");
            int idx = fileIndex(line, trim(arg.substr(0, eq)));
            if (idx < 0) return;
            size_t n = std::stoul(arg.substr(eq + 3));
            for (const auto& a : plan_.archives_to_extract) {
                if (a.file_index == idx) {
                    if (a.volume_indices.size() != n)
                        fail(line, "volumes " + std::to_string(a.volume_indices.size()) + " != " + std::to_string(n));
                    return;
                }
            }
            fail(line, "not an archive task (first volume expected)");
        } else if (cmd == "unsupported") {
            int idx = fileIndex(line, arg);
            if (idx < 0) return;
            for (const auto& a : plan_.archives_to_extract) {
                if (a.file_index == idx) {
                    if (a.unsupported_reason.empty()) fail(line, arg + " should be unsupported");
                    return;
                }
            }
            fail(line, arg + " is not an archive task");
        } else if (cmd == "skip") {
            int idx = fileIndex(line, arg);
            if (idx >= 0 && !plan_.skipped_files.count(idx)) {
                auto it = plan_.file_destinations.find(idx);
                fail(line, arg + " not skipped (-> " + (it == plan_.file_destinations.end() ? "?" : it->second) + ")");
            }
        } else if (cmd == "in" || cmd == "notin") {
            size_t s2 = arg.find(' ');
            if (s2 == std::string::npos) return fail(line, "syntax: " + cmd + " <variant> <path>");
            int v = variantIndex(line, arg.substr(0, s2));
            int idx = fileIndex(line, trim(arg.substr(s2 + 1)));
            if (v < 0 || idx < 0) return;
            auto files = plan_.getFilesForVariant(v);
            bool has = std::find(files.begin(), files.end(), idx) != files.end();
            if (cmd == "in" && !has) fail(line, arg + ": file not in variant");
            if (cmd == "notin" && has) fail(line, arg + ": file is in variant");
        } else if (cmd == "forwarder") {
            size_t s2 = arg.find(' ');
            if (s2 == std::string::npos) return fail(line, "syntax: forwarder <variant> <path|->");
            int v = variantIndex(line, arg.substr(0, s2));
            if (v < 0) return;
            std::string path = trim(arg.substr(s2 + 1));
            auto fw = plan_.forwardersForVariant(v);
            if (path == "-") {
                if (!fw.empty()) fail(line, "variant has a forwarder: " + fx_.files[fw[0]].name);
                return;
            }
            int idx = fileIndex(line, path);
            if (idx >= 0 && std::find(fw.begin(), fw.end(), idx) == fw.end()) fail(line, path + " is not a forwarder of the variant");
        } else {
            fail(line, "unknown check '" + cmd + "'");
        }
    }

    const Fixture& fx_;
    installer::PortInstallPlan plan_;
    int failures_ = 0;
};

// Base source with one file open at a time, like CustomEngineBackend.
class FakeTorrentSource : public datasource::IDataSource {
public:
    std::vector<std::string> files;
    int open_index = -1;
    int opens = 0;
    bool open(const std::string&, int file_index) override {
        if (open_index >= 0) return false;
        if (file_index < 0 || file_index >= static_cast<int>(files.size())) return false;
        open_index = file_index;
        ++opens;
        return true;
    }
    size_t read(uint64_t offset, void* buf, size_t size) override {
        if (open_index < 0) return 0;
        const std::string& f = files[open_index];
        if (offset >= f.size()) return 0;
        size_t n = std::min<size_t>(size, f.size() - static_cast<size_t>(offset));
        std::memcpy(buf, f.data() + offset, n);
        return n;
    }
    uint64_t totalSize() const override { return open_index >= 0 ? files[open_index].size() : 0; }
    bool isAvailable() const override { return true; }
    datasource::SourceType type() const override { return datasource::SourceType::LocalInternal; }
    void close() override { open_index = -1; }
};

void concatChecks(int& failed) {
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) {
            ++failed;
            std::cerr << "  FAIL ConcatDataSource: " << what << "\n";
        }
    };
    FakeTorrentSource base;
    base.files = {"readme", "ABCDEFGHIJ", "KLMNO", "PQRSTUVWXYZ"};
    datasource::ConcatDataSource src(&base, "hash", {{1, 10}, {2, 5}, {3, 11}});
    check(src.totalSize() == 26, "total size");
    check(src.open("", -1), "open first volume");

    // Tail first (like the 7z header), then the whole stream from the start.
    char tail[4] = {0};
    check(src.read(22, tail, 4) == 4 && std::string(tail, 4) == "WXYZ", "read tail of last volume");
    std::string all;
    uint64_t off = 0;
    char buf[7];
    int guard = 0;
    while (off < src.totalSize() && guard++ < 100) {
        size_t got = src.read(off, buf, sizeof(buf));
        if (got == 0) break;
        all.append(buf, got);
        off += got;
    }
    check(all == "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "sequential read across volumes: '" + all + "'");
    check(src.read(26, buf, 1) == 0, "read past the end");
    check(src.volumeSwitches() == base.opens, "every switch reopens the base source");
    src.close();
    check(base.open_index == -1, "close releases the base source");
}

// Stored (uncompressed) ZIP built by hand, with a comment after the directory.
std::string makeZip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, cd;
    auto u16 = [](std::string& s, uint16_t v) { s += char(v & 0xFF); s += char(v >> 8); };
    auto u32 = [&](std::string& s, uint32_t v) { u16(s, v & 0xFFFF); u16(s, v >> 16); };
    for (const auto& f : files) {
        uint32_t off = static_cast<uint32_t>(out.size());
        u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0);
        u32(out, 0); u32(out, f.second.size()); u32(out, f.second.size());
        u16(out, f.first.size()); u16(out, 0);
        out += f.first + f.second;
        u32(cd, 0x02014b50); u16(cd, 20); u16(cd, 20); u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0);
        u32(cd, 0); u32(cd, f.second.size()); u32(cd, f.second.size());
        u16(cd, f.first.size()); u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0); u32(cd, 0); u32(cd, off);
        cd += f.first;
    }
    uint32_t cdOff = static_cast<uint32_t>(out.size());
    out += cd;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, files.size()); u16(out, files.size());
    u32(out, cd.size()); u32(out, cdOff); u16(out, 7);
    out += "comment";
    return out;
}

void zipChecks(int& failed) {
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) {
            ++failed;
            std::cerr << "  FAIL parseZipDirectory: " << what << "\n";
        }
    };
    std::string zip = makeZip({{"maxpayne/", ""}, {"maxpayne/maxpayne.nro", "NRO0DATA"}, {"maxpayne\\files\\a.dat", "0123456789"}});
    size_t reads = 0;
    auto reader = [&](uint64_t off, void* buf, size_t n) {
        if (off + n > zip.size()) return false;
        std::memcpy(buf, zip.data() + off, n);
        ++reads;
        return true;
    };
    util::ArchivePeek peek;
    std::string err;
    check(util::parseZipDirectory(zip.size(), reader, peek, err), "parse: " + err);
    check(peek.entries.size() == 3, "3 entries, got " + std::to_string(peek.entries.size()));
    if (peek.entries.size() == 3) {
        check(peek.entries[0].is_dir && peek.entries[0].path == "maxpayne", "folder entry");
        check(peek.entries[1].path == "maxpayne/maxpayne.nro" && peek.entries[1].size == 8, "file entry");
        check(peek.entries[2].path == "maxpayne/files/a.dat", "backslashes normalized");
    }
    check(peek.total_uncompressed == 18, "total size");
    check(reads == 2, "only the tail and the directory are read");
    check(!util::parseZipDirectory(10, reader, peek, err), "too small archive is rejected");
}

void unitChecks(int& failed) {
    concatChecks(failed);
    zipChecks(failed);
    struct Case { const char* path; bool prot; };
    const Case cases[] = {
        {"bootloader/hekate_ipl.ini", true},
        {"atmosphere/package3", true},
        {"atmosphere/config/system_settings.ini", true},
        {"atmosphere/contents/0100000000010000/romfs/a.bin", false},
        {"Nintendo/Contents/x", true},
        {"emuMMC/emummc.ini", true},
        {"hbmenu.nro", true},
        {"payload.bin", true},
        {"switch/game/game.nro", false},
        {"maxpaynenx/config.txt", false},
        {"switch/../bootloader/x", true},
    };
    for (const auto& c : cases) {
        if (installer::isProtectedSdPath(c.path) != c.prot) {
            ++failed;
            std::cerr << "  FAIL isProtectedSdPath(" << c.path << ") != " << c.prot << "\n";
        }
    }
}

} // namespace

// --lint <dir>: plans for real file lists (fixture format, "## expect" optional)
// with warnings that point at releases needing a recipe.
int lint(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> paths;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ".txt") paths.push_back(e.path());
    }
    std::sort(paths.begin(), paths.end());
    int flagged = 0;
    for (const auto& p : paths) {
        Fixture fx;
        if (!loadFixture(p, fx)) continue;
        auto plan = installer::PortInstallPlanner::instance().plan(fx.topic, fx.title, fx.files, fx.lang);
        std::vector<std::string> warn;
        if (!plan.is_port) warn.push_back("not recognized as a port");
        int nsp = 0, nro = 0;
        for (const auto& f : fx.files) {
            std::string l = f.name;
            std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return std::tolower(c); });
            if (l.size() > 4 && l.compare(l.size() - 4, 4, ".nsp") == 0) ++nsp;
            if (l.size() > 4 && l.compare(l.size() - 4, 4, ".nro") == 0) ++nro;
        }
        if (plan.port_game_name.find('.') != std::string::npos) warn.push_back("game folder looks like a file: " + plan.port_game_name);
        if (nsp > 1) warn.push_back(std::to_string(nsp) + " NSPs");
        if (nro == 0 && plan.archives_to_extract.empty()) warn.push_back("no .nro and no archives");
        const auto guessed = std::count_if(plan.archives_to_extract.begin(), plan.archives_to_extract.end(),
                                           [](const installer::PortArchiveTask& a) { return !a.from_recipe; });
        if (guessed > 0) {
            warn.push_back(std::to_string(guessed) + " archive(s): target decided from their listing at install time");
        }
        for (const auto& a : plan.archives_to_extract) {
            if (!a.unsupported_reason.empty()) warn.push_back("unsupported: " + a.archive_torrent_path);
        }
        for (size_t v = 0; v < plan.variants.size(); ++v) {
            bool hasNroDest = false;
            for (int idx : plan.getFilesForVariant(static_cast<int>(v))) {
                auto it = plan.file_destinations.find(idx);
                if (it != plan.file_destinations.end() && it->second.size() > 4 &&
                    it->second.compare(it->second.size() - 4, 4, ".nro") == 0) hasNroDest = true;
            }
            if (!plan.forwardersForVariant(static_cast<int>(v)).empty() && !hasNroDest && plan.getArchivesForVariant(static_cast<int>(v)).empty())
                warn.push_back("variant " + plan.variants[v].id + ": shortcut but no .nro");
        }
        for (const auto& s : plan.skipped_files) {
            if (s.second != "readme/screenshot") {
                warn.push_back("skipped #" + std::to_string(s.first) + " (" + s.second + ")");
                break;
            }
        }
        std::string ids;
        for (const auto& v : plan.variants) ids += (ids.empty() ? "" : ",") + v.id;
        const bool needsLook = std::any_of(warn.begin(), warn.end(), [](const std::string& w) {
            return w.find("archive(s)") == std::string::npos;
        });
        if (needsLook) ++flagged;
        std::cout << (needsLook ? "CHECK " : "ok    ") << p.filename().string() << "  game=" << plan.port_game_name
                  << " variants=" << ids << (plan.recipe_id.empty() ? "" : " recipe") << "\n";
        for (const auto& w : warn) std::cout << "        - " << w << "\n";
    }
    std::cout << flagged << " of " << paths.size() << " releases need a look\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--lint") return lint(argv[2]);
    std::filesystem::path dir = argc > 1 ? argv[1] : "pctest/port_fixtures";
    std::vector<std::filesystem::path> paths;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ".txt") paths.push_back(e.path());
    }
    std::sort(paths.begin(), paths.end());
    if (paths.empty()) {
        std::cerr << "no fixtures in " << dir.string() << "\n";
        return 1;
    }

    int failed = 0;
    unitChecks(failed);
    for (const auto& p : paths) {
        Fixture fx;
        if (!loadFixture(p, fx)) {
            ++failed;
            continue;
        }
        Checker c(fx);
        bool ok = c.run();
        std::cout << (ok ? "ok    " : "FAIL  ") << fx.file << " (" << fx.files.size() << " files, "
                  << fx.expects.size() << " checks)\n";
        if (!ok) ++failed;
    }
    std::cout << (failed ? "FAILED: " + std::to_string(failed) : std::string("ALL PORT PLANNER TESTS PASSED")) << "\n";
    return failed ? 1 : 0;
}
