#include "torrent_manager.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "../net/http_client.h"
#include "../utils/log.h"

namespace torrent {

static bool g_logged_list_sample = false;

static bool pathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool ensureDirRecursiveImpl(const std::string& path) {
    if (path.empty()) return false;
    if (pathExists(path)) return true;

    std::string cur;
    size_t pos = 0;
    if (path.rfind("sdmc:/", 0) == 0) {
        cur = "sdmc:/";
        pos = 6;
    }

    while (pos < path.size()) {
        size_t next = path.find('/', pos);
        std::string part = (next == std::string::npos) ? path.substr(pos) : path.substr(pos, next - pos);
        if (!part.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += "/";
            cur += part;
            if (!pathExists(cur)) {
                mkdir(cur.c_str(), 0777);
            }
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return pathExists(path);
}

static std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string streamRouteName(const std::string& raw_name, const std::string& fallback) {
    std::string name = trim(raw_name);
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    if (name.empty()) {
        name = fallback;
    }
    return name;
}

static bool hasExt(const std::string& name, const std::string& ext) {
    if (name.size() < ext.size()) return false;
    std::string tail = name.substr(name.size() - ext.size());
    std::string lower_tail = tail;
    std::string lower_ext = ext;
    std::transform(lower_tail.begin(), lower_tail.end(), lower_tail.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return lower_tail == lower_ext;
}

static std::string normalizeRemoteId(const std::string& id) {
    std::string out = trim(id);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

static std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

static std::string extractBtihHash(const std::string& magnet) {
    const std::string needle = "xt=urn:btih:";
    auto p = magnet.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();

    auto e = magnet.find('&', p);
    std::string hash = (e == std::string::npos) ? magnet.substr(p) : magnet.substr(p, e - p);
    if (hash.empty()) return "";

    std::transform(hash.begin(), hash.end(), hash.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return hash;
}

TorrentManager::TorrentManager() {
    config_dir_ = "sdmc:/switch/TorrentShopNX";
    download_dir_ = "sdmc:/switch/TorrentShopNX/downloads";
    config_file_ = config_dir_ + "/torrserver.conf";
    torrserver_url_ = "http://127.0.0.1:8090";

    ensureDirs();
    loadConfig();

    server_reachable_ = pingServer();
    if (server_reachable_) {
        util::logLine("torrent: TorrServer API reachable at " + torrserver_url_);
    } else {
        util::logLine("torrent: TorrServer API is not reachable at " + torrserver_url_);
    }
}

TorrentManager::~TorrentManager() = default;

void TorrentManager::ensureDirs() const {
    ensureDirRecursiveImpl(config_dir_);
    ensureDirRecursiveImpl(download_dir_);
}

void TorrentManager::loadConfig() {
    std::ifstream in(config_file_);
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (key == "torrserver_url" && !value.empty()) {
            torrserver_url_ = value;
            if (!torrserver_url_.empty() && torrserver_url_.back() == '/') {
                torrserver_url_.pop_back();
            }
        } else if (key == "download_dir" && !value.empty()) {
            download_dir_ = value;
        }
    }
}

void TorrentManager::setServerUrl(const std::string& url) {
    if (url.empty()) return;
    
    std::string new_url = url;
    // Add http:// if missing
    if (new_url.find("://") == std::string::npos) {
        new_url = "http://" + new_url;
    }
    
    // Remove trailing slash
    while (!new_url.empty() && new_url.back() == '/') {
        new_url.pop_back();
    }
    
    if (torrserver_url_ != new_url) {
        torrserver_url_ = new_url;
        util::logLine("torrent: server URL updated to: " + torrserver_url_);
        
        // Re-ping server with new URL
        server_reachable_ = pingServer();
        if (server_reachable_) {
            util::logLine("torrent: new server URL is reachable");
        } else {
            util::logLine("torrent: new server URL is not reachable");
        }
    }
}

bool TorrentManager::pingServer() {
    net::HttpClient http;
    http.setTimeout(2);
    auto echo = http.httpGet(buildUrl("/echo"));
    if (echo.status_code == 200) return true;

    auto root = http.httpGet(buildUrl("/"));
    return root.status_code == 200;
}

int TorrentManager::allocateLocalId() {
    return next_local_id_++;
}

std::string TorrentManager::buildUrl(const std::string& path) const {
    if (path.empty()) return torrserver_url_;
    if (!path.empty() && path[0] == '/') return torrserver_url_ + path;
    return torrserver_url_ + "/" + path;
}

std::string TorrentManager::urlEncode(const std::string& value) const {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string TorrentManager::extractJsonValue(const std::string& obj, const std::string& key) const {
    const std::string needle = "\"" + key + "\"";
    auto kpos = obj.find(needle);
    if (kpos == std::string::npos) return "";

    auto colon = obj.find(':', kpos + needle.size());
    if (colon == std::string::npos) return "";

    auto q1 = obj.find('"', colon + 1);
    if (q1 == std::string::npos) return "";

    std::string value;
    bool esc = false;
    for (size_t i = q1 + 1; i < obj.size(); ++i) {
        char ch = obj[i];
        if (esc) {
            value.push_back(ch);
            esc = false;
            continue;
        }
        if (ch == '\\') {
            esc = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return "";
}

double TorrentManager::extractJsonNumber(const std::string& obj, const std::string& key) const {
    const std::string needle = "\"" + key + "\"";
    auto kpos = obj.find(needle);
    if (kpos == std::string::npos) return NAN;

    auto colon = obj.find(':', kpos + needle.size());
    if (colon == std::string::npos) return NAN;

    size_t pos = colon + 1;
    while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos]))) ++pos;

    size_t end = pos;
    while (end < obj.size()) {
        char c = obj[end];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
            ++end;
        } else {
            break;
        }
    }

    if (end == pos) return NAN;
    return std::strtod(obj.substr(pos, end - pos).c_str(), nullptr);
}

bool TorrentManager::extractJsonBool(const std::string& obj, const std::string& key, bool default_value) const {
    const std::string needle = "\"" + key + "\"";
    auto kpos = obj.find(needle);
    if (kpos == std::string::npos) return default_value;

    auto colon = obj.find(':', kpos + needle.size());
    if (colon == std::string::npos) return default_value;

    size_t pos = colon + 1;
    while (pos < obj.size() && std::isspace(static_cast<unsigned char>(obj[pos]))) ++pos;

    if (obj.compare(pos, 4, "true") == 0) return true;
    if (obj.compare(pos, 5, "false") == 0) return false;
    return default_value;
}

std::vector<std::string> TorrentManager::splitTopLevelObjects(const std::string& json) const {
    std::vector<std::string> out;
    int depth = 0;
    bool in_string = false;
    bool esc = false;
    size_t start = std::string::npos;

    for (size_t i = 0; i < json.size(); ++i) {
        char c = json[i];
        if (in_string) {
            if (esc) {
                esc = false;
                continue;
            }
            if (c == '\\') {
                esc = true;
                continue;
            }
            if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            continue;
        }

        if (c == '{') {
            if (depth == 0) start = i;
            ++depth;
            continue;
        }

        if (c == '}') {
            if (depth > 0) {
                --depth;
                if (depth == 0 && start != std::string::npos) {
                    out.push_back(json.substr(start, i - start + 1));
                    start = std::string::npos;
                }
            }
        }
    }

    return out;
}

bool TorrentManager::addMagnetViaApi(const std::string& magnet, std::string& out_remote_id) {
    if (!server_reachable_) {
        server_reachable_ = pingServer();
    }

    if (!server_reachable_) {
        std::string btih = extractBtihHash(magnet);
        if (!btih.empty()) {
            out_remote_id = normalizeRemoteId(btih);
            util::logLine("torrent: add fallback using BTIH id=" + out_remote_id + " (server unreachable)");
            return true;
        }
        return false;
    }

    net::HttpClient http;
    http.setTimeout(2);
    const std::string escaped = jsonEscape(magnet);

    const std::vector<std::string> json_bodies = {
        std::string("{\"link\":\"") + escaped + "\"}",
        std::string("{\"Link\":\"") + escaped + "\"}",
        std::string("{\"url\":\"") + escaped + "\"}",
        std::string("{\"Url\":\"") + escaped + "\"}",
        std::string("{\"magnet\":\"") + escaped + "\"}"
    };

    std::vector<net::HttpResponse> tries;
    // TorrServer vMatriX/v124+ style API
    tries.push_back(http.httpPost(buildUrl("/torrents"),
                                  std::string("{\"action\":\"add\",\"link\":\"") + escaped + "\"}"));
    for (const auto& b : json_bodies) {
        tries.push_back(http.httpPost(buildUrl("/torrent/add"), b));
    }
    tries.push_back(http.httpGet(buildUrl("/torrent/add?link=") + urlEncode(magnet)));
    tries.push_back(http.httpGet(buildUrl("/torrent/add?url=") + urlEncode(magnet)));
    tries.push_back(http.httpGet(buildUrl("/torrent/add?magnet=") + urlEncode(magnet)));

    for (const auto& r : tries) {
        util::logLine("torrent: add try status=" + std::to_string(r.status_code));
        if (r.status_code < 200 || r.status_code >= 300) continue;

        std::string id = extractJsonValue(r.body, "hash");
        if (id.empty()) id = extractJsonValue(r.body, "Hash");
        if (id.empty()) id = extractJsonValue(r.body, "id");
        if (id.empty()) id = extractJsonValue(r.body, "Id");
        if (id.empty()) {
            double n = extractJsonNumber(r.body, "id");
            if (std::isfinite(n) && n >= 0.0) id = std::to_string(static_cast<int>(n));
        }
        if (id.empty()) {
            std::string b = trim(r.body);
            if (!b.empty() && b.front() == '"' && b.back() == '"' && b.size() >= 2) {
                b = b.substr(1, b.size() - 2);
            }
            if (!b.empty()) id = b;
        }

        if (!id.empty()) {
            out_remote_id = normalizeRemoteId(id);
            util::logLine("torrent: add accepted, remote id=" + out_remote_id);
            return true;
        }
    }

    bool any_success_status = false;
    for (const auto& r : tries) {
        if (r.status_code >= 200 && r.status_code < 300) {
            any_success_status = true;
            break;
        }
    }
    if (!any_success_status) {
        server_reachable_ = false;
    }

    // Fallback: many APIs use BTIH hash as remote id; can work even when /add response differs.
    std::string btih = extractBtihHash(magnet);
    if (!btih.empty()) {
        out_remote_id = normalizeRemoteId(btih);
        util::logLine("torrent: add fallback using BTIH id=" + out_remote_id);
        return true;
    }

    return false;
}

bool TorrentManager::removeViaApi(const std::string& remote_id) {
    net::HttpClient http;
    std::vector<net::HttpResponse> post_tries = {
        http.httpPost(buildUrl("/torrents"),
                      std::string("{\"action\":\"rem\",\"hash\":\"") + remote_id + "\"}"),
        http.httpPost(buildUrl("/torrents"),
                      std::string("{\"action\":\"drop\",\"hash\":\"") + remote_id + "\"}")
    };
    for (const auto& r : post_tries) {
        if (r.status_code >= 200 && r.status_code < 300) return true;
    }

    std::vector<std::string> urls = {
        buildUrl("/torrent/rem?hash=") + urlEncode(remote_id),
        buildUrl("/torrent/rem?id=") + urlEncode(remote_id),
        buildUrl("/torrent/remove?hash=") + urlEncode(remote_id),
        buildUrl("/torrent/remove?id=") + urlEncode(remote_id)
    };

    for (const auto& u : urls) {
        auto res = http.httpGet(u);
        if (res.status_code >= 200 && res.status_code < 300) {
            return true;
        }
    }
    return false;
}

bool TorrentManager::actionViaApi(const std::string& remote_id, const std::vector<std::string>& actions) {
    net::HttpClient http;
    for (const auto& a : actions) {
        auto by_hash = http.httpGet(buildUrl("/torrent/") + a + "?hash=" + urlEncode(remote_id));
        if (by_hash.status_code >= 200 && by_hash.status_code < 300) return true;

        auto by_id = http.httpGet(buildUrl("/torrent/") + a + "?id=" + urlEncode(remote_id));
        if (by_id.status_code >= 200 && by_id.status_code < 300) return true;
    }
    return false;
}

bool TorrentManager::getTorrentDetails(const std::string& remote_id, std::string& out_body) {
    if (!server_reachable_) return false;

    net::HttpClient http;
    http.setTimeout(1);
    // TorrServer modern API
    auto post = http.httpPost(buildUrl("/torrents"),
                              std::string("{\"action\":\"get\",\"hash\":\"") + remote_id + "\"}");
    if (post.status_code >= 200 && post.status_code < 300 && !post.body.empty()) {
        out_body = post.body;
        return true;
    }

    std::vector<std::string> urls = {
        buildUrl("/torrent/get?hash=") + urlEncode(remote_id),
        buildUrl("/torrent/get?id=") + urlEncode(remote_id),
        buildUrl("/torrent/files?hash=") + urlEncode(remote_id),
        buildUrl("/torrent/files?id=") + urlEncode(remote_id)
    };

    for (const auto& u : urls) {
        auto res = http.httpGet(u);
        if (res.status_code >= 200 && res.status_code < 300 && !res.body.empty()) {
            out_body = res.body;
            return true;
        }
    }

    return false;
}

bool TorrentManager::parseAndCacheList(const std::string& body, std::vector<TorrentInfo>& out_list) {
    if (!g_logged_list_sample && !body.empty()) {
        std::string sample = body.substr(0, body.size() > 800 ? 800 : body.size());
        util::logLine("torrent: list sample: " + sample);
        g_logged_list_sample = true;
    }

    auto objs = splitTopLevelObjects(body);
    if (objs.empty()) return false;

    out_list.clear();
    for (const auto& obj : objs) {
        std::string remote_id = extractJsonValue(obj, "hash");
        if (remote_id.empty()) remote_id = extractJsonValue(obj, "Hash");
        if (remote_id.empty()) remote_id = extractJsonValue(obj, "id");
        if (remote_id.empty()) remote_id = extractJsonValue(obj, "Id");
        if (remote_id.empty()) {
            double id_num = extractJsonNumber(obj, "id");
            if (std::isfinite(id_num) && id_num >= 0.0) {
                remote_id = std::to_string(static_cast<int>(id_num));
            }
        }
        remote_id = normalizeRemoteId(remote_id);
        if (remote_id.empty()) continue;

        int local_id = -1;
        for (const auto& it : refs_) {
            if (it.second.remote_id == remote_id) {
                local_id = it.first;
                break;
            }
        }
        if (local_id < 0) {
            local_id = allocateLocalId();
            refs_[local_id] = TorrentRef{local_id, remote_id};
        }

        std::string name = extractJsonValue(obj, "name");
        if (name.empty()) name = extractJsonValue(obj, "Name");
        if (name.empty()) name = extractJsonValue(obj, "title");
        if (name.empty()) name = extractJsonValue(obj, "Title");

        double loaded = extractJsonNumber(obj, "loaded_size");
        double total  = extractJsonNumber(obj, "torrent_size");
        double read_useful = extractJsonNumber(obj, "bytes_read_useful_data");
        double read_data   = extractJsonNumber(obj, "bytes_read_data");
        double read_any    = extractJsonNumber(obj, "bytes_read");
        double loaded_estimate = 0.0;
        if (std::isfinite(loaded) && loaded > loaded_estimate) loaded_estimate = loaded;
        if (std::isfinite(read_useful) && read_useful > loaded_estimate) loaded_estimate = read_useful;
        if (std::isfinite(read_data) && read_data > loaded_estimate) loaded_estimate = read_data;
        if (std::isfinite(read_any) && read_any > loaded_estimate) loaded_estimate = read_any;

        double p = extractJsonNumber(obj, "progress");
        if (!std::isfinite(p)) p = extractJsonNumber(obj, "Progress");
        if (!std::isfinite(p)) p = extractJsonNumber(obj, "percent");
        if (!std::isfinite(p)) p = extractJsonNumber(obj, "Percent");
        if (!std::isfinite(p)) p = extractJsonNumber(obj, "percent_done");
        if (!std::isfinite(p)) p = extractJsonNumber(obj, "percentDone");
        if (!std::isfinite(p)) {
            if (std::isfinite(loaded) && std::isfinite(total) && total > 0.0) {
                // TorrServer reports loaded_size as BytesCompleted (piece-based),
                // which may stay zero for a while. Use read counters as smoother fallback.
                p = loaded_estimate / total;
            }
        }
        if (!std::isfinite(p)) p = 0.0;
        if (p > 1.0) p /= 100.0;
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;

        double s = extractJsonNumber(obj, "download_speed_kbps");
        bool speed_is_bytes_per_sec = false;
        if (!std::isfinite(s)) s = extractJsonNumber(obj, "downloadSpeedKbps");
        if (!std::isfinite(s)) {
            s = extractJsonNumber(obj, "download_speed");
            if (std::isfinite(s)) speed_is_bytes_per_sec = true;
        }
        if (!std::isfinite(s)) {
            s = extractJsonNumber(obj, "DownloadSpeed");
            if (std::isfinite(s)) speed_is_bytes_per_sec = true;
        }
        if (!std::isfinite(s)) s = extractJsonNumber(obj, "speed");
        if (!std::isfinite(s)) s = extractJsonNumber(obj, "Speed");
        if (!std::isfinite(s) || s < 0.0) s = 0.0;
        if (speed_is_bytes_per_sec) {
            s /= 1024.0;
        } else if (s > 4096.0) {
            s /= 1024.0;
        }

        double peers_val = extractJsonNumber(obj, "active_peers");
        if (!std::isfinite(peers_val)) peers_val = extractJsonNumber(obj, "peers");
        if (!std::isfinite(peers_val)) peers_val = extractJsonNumber(obj, "ActivePeers");
        if (!std::isfinite(peers_val) || peers_val < 0.0) peers_val = 0.0;

        double seeds_val = extractJsonNumber(obj, "active_seeds");
        if (!std::isfinite(seeds_val)) seeds_val = extractJsonNumber(obj, "seeds");
        if (!std::isfinite(seeds_val)) seeds_val = extractJsonNumber(obj, "ActiveSeeds");
        if (!std::isfinite(seeds_val) || seeds_val < 0.0) seeds_val = 0.0;

        double dht_val = extractJsonNumber(obj, "dht_nodes");
        if (!std::isfinite(dht_val)) dht_val = extractJsonNumber(obj, "dht");
        if (!std::isfinite(dht_val)) dht_val = extractJsonNumber(obj, "DhtNodes");
        if (!std::isfinite(dht_val) || dht_val < 0.0) dht_val = 0.0;

        TorrentInfo info;
        info.id = local_id;
        info.name = name;
        info.hash = remote_id;
        info.percent_done = static_cast<float>(p);
        info.download_speed_kbps = static_cast<float>(s);
        info.loaded_size = static_cast<unsigned long long>(loaded_estimate > 0.0 ? loaded_estimate : 0.0);
        info.torrent_size = static_cast<unsigned long long>((std::isfinite(total) && total > 0.0) ? total : 0.0);
        info.seeds = static_cast<int>(seeds_val);
        info.peers = static_cast<int>(peers_val);
        double known_val = extractJsonNumber(obj, "known_peers");
        info.known_peers = std::isfinite(known_val) && known_val >= 0.0 ? static_cast<int>(known_val) : 0;
        info.dht = static_cast<int>(dht_val);
        out_list.push_back(info);
    }

    return !out_list.empty();
}

bool TorrentManager::resolveRemoteId(int local_id, std::string& out_remote_id) {
    auto it = refs_.find(local_id);
    if (it != refs_.end() && !it->second.remote_id.empty()) {
        out_remote_id = normalizeRemoteId(it->second.remote_id);
        return true;
    }

    auto list = getTorrentList();
    (void)list;

    it = refs_.find(local_id);
    if (it != refs_.end() && !it->second.remote_id.empty()) {
        out_remote_id = normalizeRemoteId(it->second.remote_id);
        return true;
    }

    return false;
}

int TorrentManager::addMagnet(const std::string& magnet) {
    std::string remote_id;
    if (!addMagnetViaApi(magnet, remote_id)) {
        util::logLine("torrent: error adding magnet via API");
        return -1;
    }
    remote_id = normalizeRemoteId(remote_id);

    int local_id = allocateLocalId();
    refs_[local_id] = TorrentRef{local_id, remote_id};
    return local_id;
}

std::vector<TorrentInfo> TorrentManager::getTorrentList() {
    std::vector<TorrentInfo> list;

    if (!server_reachable_) {
        return list;
    }

    net::HttpClient http;
    http.setTimeout(2);
    auto post = http.httpPost(buildUrl("/torrents"), "{\"action\":\"list\"}");
    if (post.status_code >= 200 && post.status_code < 300 && !post.body.empty()) {
        if (parseAndCacheList(post.body, list)) {
            return list;
        }
    }

    std::vector<std::string> urls = {
        buildUrl("/torrent/list"),
        buildUrl("/torrents"),
        buildUrl("/torrent")
    };

    for (const auto& u : urls) {
        auto res = http.httpGet(u);
        if (res.status_code < 200 || res.status_code >= 300 || res.body.empty()) {
            continue;
        }
        if (parseAndCacheList(res.body, list)) {
            return list;
        }
    }

    return list;
}

float TorrentManager::getTorrentProgress(int id) {
    auto list = getTorrentList();
    for (const auto& t : list) {
        if (t.id == id) return t.percent_done;
    }
    return 0.0f;
}

bool TorrentManager::pauseTorrent(int id) {
    std::string remote_id;
    if (!resolveRemoteId(id, remote_id)) return false;
    return actionViaApi(remote_id, {"pause", "stop"});
}

bool TorrentManager::resumeTorrent(int id) {
    std::string remote_id;
    if (!resolveRemoteId(id, remote_id)) return false;
    return actionViaApi(remote_id, {"resume", "start"});
}

bool TorrentManager::cancelTorrent(int id) {
    std::string remote_id;
    if (!resolveRemoteId(id, remote_id)) return false;
    return removeViaApi(remote_id);
}

bool TorrentManager::getTorrentFiles(int id, std::vector<TorrentFileInfo>& out_files) {
    out_files.clear();

    std::string remote_id;
    if (!resolveRemoteId(id, remote_id)) return false;

    std::string body;
    if (!getTorrentDetails(remote_id, body)) return false;

    auto parse_file_objects = [&](const std::vector<std::string>& objs, bool require_path) -> bool {
        int fallback_index = 0;
        for (const auto& obj : objs) {
            std::string path = extractJsonValue(obj, "path");
            if (path.empty()) path = extractJsonValue(obj, "Path");

            if (require_path && path.empty()) continue;

            double index_n = extractJsonNumber(obj, "id");
            if (!std::isfinite(index_n)) index_n = extractJsonNumber(obj, "index");
            const bool has_explicit_index = std::isfinite(index_n);
            if (!std::isfinite(index_n)) index_n = static_cast<double>(fallback_index);

            double size_n = extractJsonNumber(obj, "length");
            if (!std::isfinite(size_n)) size_n = extractJsonNumber(obj, "size");
            if (!std::isfinite(size_n)) size_n = extractJsonNumber(obj, "Size");
            const bool has_explicit_size = std::isfinite(size_n);
            if (!std::isfinite(size_n)) size_n = 0.0;

            bool looks_like_file = !path.empty() || has_explicit_size || has_explicit_index;
            if (!looks_like_file) continue;

            std::string name = path;
            if (name.empty() && !require_path) {
                name = extractJsonValue(obj, "name");
                if (name.empty()) name = extractJsonValue(obj, "Name");
            }
            if (name.empty()) continue;

            bool wanted = extractJsonBool(obj, "wanted", true);
            wanted = extractJsonBool(obj, "Wanted", wanted);
            wanted = extractJsonBool(obj, "selected", wanted);
            wanted = extractJsonBool(obj, "Selected", wanted);

            TorrentFileInfo fi;
            fi.index = static_cast<int>(index_n);
            fi.name = name;
            fi.size = static_cast<unsigned long long>(size_n > 0.0 ? size_n : 0.0);
            fi.wanted = wanted;
            out_files.push_back(fi);
            ++fallback_index;
        }
        return !out_files.empty();
    };

    auto extract_array_for_key = [&](const std::string& payload,
                                     const std::string& key,
                                     std::string& out_array) -> bool {
        auto key_pos = payload.find(key);
        if (key_pos == std::string::npos) return false;
        auto lb = payload.find('[', key_pos + key.size());
        if (lb == std::string::npos) return false;

        size_t rb = std::string::npos;
        int depth = 0;
        bool in_string = false;
        bool esc = false;
        for (size_t i = lb; i < payload.size(); ++i) {
            char c = payload[i];
            if (in_string) {
                if (esc) {
                    esc = false;
                } else if (c == '\\') {
                    esc = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
                continue;
            }
            if (c == '[') {
                ++depth;
            } else if (c == ']') {
                --depth;
                if (depth == 0) {
                    rb = i;
                    break;
                }
            }
        }

        if (rb == std::string::npos || rb <= lb) return false;
        out_array = payload.substr(lb, rb - lb + 1);
        return true;
    };

    auto parse_array_for_key = [&](const std::string& payload, const std::string& key) -> bool {
        std::string arr;
        if (!extract_array_for_key(payload, key, arr)) return false;
        auto file_objs = splitTopLevelObjects(arr);
        if (file_objs.empty()) return false;
        if (parse_file_objects(file_objs, true)) return true;
        out_files.clear();
        return false;
    };

    // Preferred formats from various TorrServer API versions.
    if (parse_array_for_key(body, "\"file_stats\"")) return true;
    if (parse_array_for_key(body, "\"Files\"")) return true;
    if (parse_array_for_key(body, "\"files\"")) return true;

    // Some APIs return JSON where file list is escaped into "data":"{...}".
    auto objs = splitTopLevelObjects(body);
    for (const auto& obj : objs) {
        std::string data = extractJsonValue(obj, "data");
        if (data.empty()) data = extractJsonValue(obj, "Data");
        if (data.empty()) continue;

        if (parse_array_for_key(data, "\"file_stats\"")) return true;
        if (parse_array_for_key(data, "\"Files\"")) return true;
        if (parse_array_for_key(data, "\"files\"")) return true;
    }

    // Last-resort fallback for endpoints that directly return file objects.
    return parse_file_objects(objs, false);
}

bool TorrentManager::setFileWanted(int id, int file_index, bool wanted) {
    std::string remote_id;
    if (!resolveRemoteId(id, remote_id)) return false;

    net::HttpClient http;
    const std::string wanted_str = wanted ? "1" : "0";

    std::vector<std::string> urls = {
        buildUrl("/torrent/set?hash=") + urlEncode(remote_id) + "&file=" + std::to_string(file_index) + "&wanted=" + wanted_str,
        buildUrl("/torrent/set?id=") + urlEncode(remote_id) + "&file=" + std::to_string(file_index) + "&wanted=" + wanted_str,
        buildUrl("/torrent/select?hash=") + urlEncode(remote_id) + "&file=" + std::to_string(file_index) + "&selected=" + wanted_str,
        buildUrl("/torrent/select?id=") + urlEncode(remote_id) + "&file=" + std::to_string(file_index) + "&selected=" + wanted_str
    };

    for (const auto& u : urls) {
        auto res = http.httpGet(u);
        if (res.status_code >= 200 && res.status_code < 300) {
            return true;
        }
    }

    return false;
}

bool TorrentManager::preloadTorrentFile(int id, int file_index, const std::string& stream_name) {
    std::string remote_id;
    if (!resolveRemoteId(id, remote_id)) return false;
    if (file_index < 0) return false;

    net::HttpClient http;
    http.setTimeout(3);

    const std::string route_name = streamRouteName(stream_name, remote_id);
    // TorrServe Android client triggers preload via /stream with short timeout.
    std::string url = buildUrl("/stream/" + urlEncode(route_name) + "?link=") + urlEncode(remote_id) +
                      "&index=" + std::to_string(file_index) + "&preload";
    auto res = http.httpGet(url);
    util::logLine("torrent: preload request status=" + std::to_string(res.status_code) +
                  " hash=" + remote_id + " index=" + std::to_string(file_index) +
                  " route=" + route_name);

    // Timeout/error can still mean preload started on server side.
    return res.status_code == 200 || res.status_code == 206 || res.status_code == 0;
}

size_t TorrentManager::pumpStreamRead(int id, int file_index, uint64_t offset, size_t max_bytes, const std::string& stream_name) {
    std::string remote_id;
    if (!resolveRemoteId(id, remote_id)) return 0;
    if (file_index < 0 || max_bytes == 0) return 0;

    net::HttpClient http;
    http.setTimeout(3);
    http.setKeepAlive(true);

    const std::string route_name = streamRouteName(stream_name, remote_id);
    std::string url = buildUrl("/stream/" + urlEncode(route_name) + "?link=") + urlEncode(remote_id) +
                      "&index=" + std::to_string(file_index) + "&play";

    size_t received = 0;
    int code = http.httpGetStream(url, offset, static_cast<uint64_t>(max_bytes),
                                  [&](const void*, size_t n) -> size_t {
                                      received += n;
                                      return n;
                                  });
    if ((code == 200 || code == 206 || code == 0) && received > 0) {
        return received;
    }

    return 0;
}

bool TorrentManager::getStreamFiles(int id, std::vector<std::string>& out_paths, std::string& out_name) {
    out_paths.clear();
    out_name.clear();

    std::vector<TorrentFileInfo> files;
    if (!getTorrentFiles(id, files)) return false;

    // Keep compatibility with the existing stream installer: pick NSP/PFS0 files
    // and map them to the configured download directory.
    for (const auto& f : files) {
        if (!hasExt(f.name, ".nsp") && !hasExt(f.name, ".pfs0")) continue;

        if (out_name.empty()) out_name = f.name;

        std::string path = download_dir_;
        if (!path.empty() && path.back() != '/') path += "/";
        path += f.name;
        out_paths.push_back(path);
    }

    return !out_paths.empty();
}

} // namespace torrent

// Определение getTorrentHash вынесено сюда, после всех методов
bool torrent::TorrentManager::getTorrentHash(int id, std::string& out_hash) {
    // Сначала проверяем локальный кеш refs_
    auto it = refs_.find(id);
    if (it != refs_.end() && !it->second.remote_id.empty()) {
        out_hash = it->second.remote_id;
        return true;
    }

    // Пробуем обновить список и повторно проверить
    auto list = getTorrentList();
    for (const auto& t : list) {
        if (t.id == id && !t.hash.empty()) {
            out_hash = t.hash;
            return true;
        }
    }

    return false;
}
