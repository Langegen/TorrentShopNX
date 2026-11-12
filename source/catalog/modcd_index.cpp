#include "modcd_index.hpp"

#include "../utils/atomic_file.h"
#include "homebrew_fs.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace modcd {

namespace {

using homebrew::fsx::ensureDir;
using homebrew::fsx::fileExists;
using homebrew::fsx::join;
using homebrew::fsx::readFile;
using json = nlohmann::json;

constexpr const char* kFile = "modcd.json";
constexpr const char* kStampFile = "modcd_last_check";

std::string trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    return s.substr(b);
}

// 16 hex digits -> application ID; 0 for anything else.
uint64_t parseTitleId(const std::string& s) {
    if (s.size() != 16) return 0;
    uint64_t v = 0;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return 0;
        v = (v << 4) | static_cast<uint64_t>(std::isdigit(static_cast<unsigned char>(c))
                                                 ? c - '0'
                                                 : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
    }
    return v;
}

}  // namespace

bool parseIndex(const std::string& text, TitleMap& out, std::string& err) {
    out.clear();
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) {
        err = "not a JSON object";
        return false;
    }
    if (j.value("format", 0) != 1) {
        err = "unknown format";
        return false;
    }
    const auto titles = j.find("titles");
    if (titles == j.end() || !titles->is_object()) {
        err = "no titles";
        return false;
    }
    for (auto it = titles->begin(); it != titles->end(); ++it) {
        const uint64_t tid = parseTitleId(it.key());
        if (tid == 0 || !it.value().is_object()) continue;
        TitleMods m;
        const json& v = it.value();
        if (v.contains("mods") && v["mods"].is_number_integer()) m.mods = v["mods"].get<int>();
        if (m.mods <= 0) continue;
        if (v.contains("versions") && v["versions"].is_array()) {
            for (const json& n : v["versions"]) {
                if (n.is_number_unsigned()) m.versions.push_back(static_cast<uint32_t>(n.get<uint64_t>()));
            }
        }
        if (v.contains("types") && v["types"].is_array()) {
            for (const json& t : v["types"]) {
                if (t.is_string()) m.types.push_back(t.get<std::string>());
            }
        }
        out[tid & ~0x1FFFULL] = std::move(m);
    }
    if (out.empty()) {
        err = "no games";
        return false;
    }
    return true;
}

bool fitsVersion(const TitleMods& mods, uint32_t installed_patch_version) {
    return std::find(mods.versions.begin(), mods.versions.end(), installed_patch_version) != mods.versions.end();
}

Index::Index(std::string cache_dir, std::string url, homebrew::Fetcher fetch, homebrew::Clock now,
             long long refresh_interval_sec)
    : cache_dir_(std::move(cache_dir)), url_(std::move(url)), fetch_(std::move(fetch)), now_(std::move(now)),
      interval_(refresh_interval_sec), titles_(std::make_shared<TitleMap>()) {}

bool Index::loadCached() {
    std::string text, err;
    auto map = std::make_shared<TitleMap>();
    if (!readFile(join(cache_dir_, kFile), text) || !parseIndex(text, *map, err)) return false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        titles_ = std::move(map);
    }
    loaded_.store(true);
    return true;
}

bool Index::refresh(bool force) {
    std::lock_guard<std::mutex> guard(refresh_mutex_);
    const long long now = now_ ? now_() : 0;
    const long long last = lastCheck();
    if (!force && loaded_.load() && last > 0 && now >= last && now - last < interval_) return true;

    ensureDir(cache_dir_);
    const std::string path = join(cache_dir_, kFile);
    const std::string etag_path = path + ".etag";
    std::string etag;
    if (fileExists(path) && readFile(etag_path, etag)) etag = trimmed(etag);

    const homebrew::FetchResult r = fetch_ ? fetch_(url_, etag) : homebrew::FetchResult{};
    bool ok = r.status == 304 && fileExists(path);
    if (!ok && r.status == 200) {
        TitleMap probe;
        std::string err;
        if (parseIndex(r.body, probe, err) && util::atomicWriteFile(path, r.body)) {
            if (r.etag.empty()) std::remove(etag_path.c_str());
            else util::atomicWriteFile(etag_path, r.etag);
            ok = true;
            loaded_.store(false);  // read the new file below
        }
    }
    if (!loaded_.load()) loadCached();
    if (ok) util::atomicWriteFile(join(cache_dir_, kStampFile), std::to_string(now));
    return loaded_.load();
}

bool Index::find(uint64_t title_id, TitleMods& out) const {
    std::shared_ptr<const TitleMap> map;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        map = titles_;
    }
    const auto it = map->find(title_id & ~0x1FFFULL);
    if (it == map->end()) return false;
    out = it->second;
    return true;
}

size_t Index::size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return titles_->size();
}

long long Index::lastCheck() const {
    std::string text;
    if (!readFile(join(cache_dir_, kStampFile), text)) return 0;
    text = trimmed(text);
    char* end = nullptr;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    return (end && end != text.c_str()) ? v : 0;
}

}  // namespace modcd
