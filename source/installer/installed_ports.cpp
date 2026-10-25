#include "installed_ports.h"

#include "utils/app_paths.h"
#include "utils/log.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <borealis/extern/nlohmann/json.hpp>

#ifdef __SWITCH__
#include <switch.h>
#include <mutex>
extern std::recursive_mutex g_switch_service_mutex;
#endif

namespace installer {

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Folders that must never be removed even when empty.
bool isKeptDir(const std::string& dir) {
    std::string d = lowerAscii(dir);
    while (!d.empty() && d.back() == '/') d.pop_back();
    return d.empty() || d == "sdmc:" || d == "sdmc:/switch" || d == "." || d == "./switch" ||
           d == "sdmc:/atmosphere" || d == "sdmc:/atmosphere/contents";
}

} // namespace

InstalledPorts& InstalledPorts::instance() {
    static InstalledPorts inst;
    return inst;
}

std::string InstalledPorts::keyFor(const std::string& topic_id, const std::string& title) {
    if (!topic_id.empty()) {
        std::string k;
        for (char c : topic_id) {
            if (std::isalnum(static_cast<unsigned char>(c))) k += c;
        }
        if (!k.empty()) return k;
    }
    std::string k;
    for (char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c))) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (k.size() >= 48) break;
    }
    return k.empty() ? "port" : "t_" + k;
}

std::string InstalledPorts::manifestPath(const std::string& key) const {
    return std::string(TSNX_DATA_DIR) + "/ports/" + key + ".json";
}

bool InstalledPorts::load(const std::string& key, InstalledPort& out) const {
    std::ifstream in(manifestPath(key), std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    out = InstalledPort();
    out.key = key;
    out.topic_id = j.value("topic_id", "");
    out.title = j.value("title", "");
    out.variant_id = j.value("variant", "");
    out.variant_title = j.value("variant_title", "");
    out.game = j.value("game", "");
    out.recipe_id = j.value("recipe", "");
    out.installed_at = static_cast<std::time_t>(j.value("installed_at", 0LL));
    if (j.contains("files") && j["files"].is_array()) {
        for (const auto& f : j["files"]) {
            if (f.is_string()) out.files.push_back(f.get<std::string>());
        }
    }
    if (j.contains("forwarders") && j["forwarders"].is_array()) {
        for (const auto& t : j["forwarders"]) {
            if (t.is_number_unsigned()) out.forwarder_tids.push_back(t.get<uint64_t>());
        }
    }
    return true;
}

std::vector<InstalledPort> InstalledPorts::list() const {
    std::vector<InstalledPort> out;
    std::error_code ec;
    const std::string dir = std::string(TSNX_DATA_DIR) + "/ports";
    if (!std::filesystem::is_directory(dir, ec)) return out;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() != ".json") continue;
        InstalledPort p;
        if (load(e.path().stem().string(), p)) out.push_back(std::move(p));
    }
    return out;
}

uint64_t InstalledPorts::sizeOnDisk(const InstalledPort& port, size_t* existing_files) {
    uint64_t total = 0;
    size_t count = 0;
    for (const auto& f : port.files) {
        struct stat st;
        if (stat(f.c_str(), &st) == 0) {
            total += static_cast<uint64_t>(st.st_size);
            ++count;
        }
    }
    if (existing_files) *existing_files = count;
    return total;
}

bool InstalledPorts::isInstalled(const std::string& topic_id, const std::string& title) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!keys_loaded_) {
        keys_loaded_ = true;
        std::error_code ec;
        const std::string dir = std::string(TSNX_DATA_DIR) + "/ports";
        if (std::filesystem::is_directory(dir, ec)) {
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (e.path().extension() == ".json") keys_.insert(e.path().stem().string());
            }
        }
    }
    return keys_.count(keyFor(topic_id, title)) > 0;
}

void InstalledPorts::rememberKey(const std::string& key, bool installed) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (installed) keys_.insert(key);
    else keys_.erase(key);
}

size_t InstalledPorts::saveReplacing(const InstalledPort& port) {
    InstalledPort fresh = port;
    if (fresh.key.empty()) fresh.key = keyFor(fresh.topic_id, fresh.title);
    std::sort(fresh.files.begin(), fresh.files.end());
    fresh.files.erase(std::unique(fresh.files.begin(), fresh.files.end()), fresh.files.end());

    size_t removed = 0;
    InstalledPort old;
    if (load(fresh.key, old)) {
        std::set<std::string> keep;
        for (const auto& f : fresh.files) keep.insert(lowerAscii(f));
        std::vector<std::string> stale;
        for (const auto& f : old.files) {
            if (!keep.count(lowerAscii(f))) stale.push_back(f);
        }
        removed = removeFilesAndEmptyDirs(withoutFilesOfOtherPorts(fresh.key, stale));
        if (removed > 0) {
            util::logLine("installed_ports: removed " + std::to_string(removed) + " files of the previous version of " + fresh.key);
        }
    }

    nlohmann::json j;
    j["topic_id"] = fresh.topic_id;
    j["title"] = fresh.title;
    j["variant"] = fresh.variant_id;
    j["variant_title"] = fresh.variant_title;
    j["game"] = fresh.game;
    j["recipe"] = fresh.recipe_id;
    j["installed_at"] = static_cast<long long>(fresh.installed_at ? fresh.installed_at : std::time(nullptr));
    j["files"] = fresh.files;
    j["forwarders"] = fresh.forwarder_tids;

    std::error_code ec;
    std::filesystem::create_directories(std::string(TSNX_DATA_DIR) + "/ports", ec);
    const std::string path = manifestPath(fresh.key);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump(1);
    }
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, ec);
    if (ec) util::logLine("installed_ports: cannot save " + path + ": " + ec.message());
    else {
        rememberKey(fresh.key, true);
        util::logLine("installed_ports: saved " + path + " (" + std::to_string(fresh.files.size()) + " files)");
    }
    return removed;
}

size_t removeFilesAndEmptyDirs(const std::vector<std::string>& paths) {
    size_t removed = 0;
    std::set<std::string> dirs;
    for (const auto& p : paths) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec) && std::filesystem::remove(p, ec)) ++removed;
        std::string d = p;
        size_t slash;
        while ((slash = d.find_last_of('/')) != std::string::npos && slash > 0) {
            d = d.substr(0, slash);
            if (isKeptDir(d)) break;
            dirs.insert(d);
        }
    }
    // Deepest first, so children go before parents.
    std::vector<std::string> ordered(dirs.begin(), dirs.end());
    std::sort(ordered.begin(), ordered.end(), [](const std::string& a, const std::string& b) {
        return std::count(a.begin(), a.end(), '/') > std::count(b.begin(), b.end(), '/');
    });
    for (const auto& d : ordered) {
        std::error_code ec;
        if (std::filesystem::is_directory(d, ec) && std::filesystem::is_empty(d, ec)) {
            std::filesystem::remove(d, ec);
        }
    }
    return removed;
}

std::vector<std::string> InstalledPorts::withoutFilesOfOtherPorts(const std::string& key,
                                                                  const std::vector<std::string>& files) const {
    std::set<std::string> shared;
    std::error_code ec;
    const std::string dir = std::string(TSNX_DATA_DIR) + "/ports";
    if (std::filesystem::is_directory(dir, ec)) {
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".json") continue;
            const std::string other = e.path().stem().string();
            if (other == key) continue;
            InstalledPort p;
            if (!load(other, p)) continue;
            for (const auto& f : p.files) shared.insert(lowerAscii(f));
        }
    }
    std::vector<std::string> out;
    for (const auto& f : files) {
        if (!shared.count(lowerAscii(f))) out.push_back(f);
    }
    return out;
}

bool InstalledPorts::uninstall(const std::string& key, size_t& removed_files, std::string& out_error) {
    removed_files = 0;
    InstalledPort port;
    if (!load(key, port)) {
        out_error = "Port is not installed";
        return false;
    }
    removed_files = removeFilesAndEmptyDirs(withoutFilesOfOtherPorts(key, port.files));
#ifdef __SWITCH__
    if (!port.forwarder_tids.empty()) {
        // ns has to be opened for the call (rc 58369 = closed service), like the library does.
        std::lock_guard<std::recursive_mutex> service_lock(g_switch_service_mutex);
        Result nsRc = nsInitialize();
        if (R_SUCCEEDED(nsRc)) {
            for (uint64_t tid : port.forwarder_tids) {
                Result rc = nsDeleteApplicationCompletely(tid);
                util::logLine("installed_ports: delete forwarder " + std::to_string(tid) + " rc=" + std::to_string(rc));
            }
            nsExit();
        } else {
            util::logLine("installed_ports: nsInitialize failed rc=" + std::to_string(nsRc));
        }
    }
#endif
    std::error_code ec;
    std::filesystem::remove(manifestPath(key), ec);
    rememberKey(key, false);
    util::logLine("installed_ports: uninstalled " + key + ", removed " + std::to_string(removed_files) + " files");
    return true;
}

} // namespace installer
