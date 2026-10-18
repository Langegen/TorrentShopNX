#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <fstream>
#include <cstdint>
#include <filesystem>

#include "../source/installer/PortInstallPlanner.hpp"

// Simple bencode decoder for .meta files to extract file paths and sizes
struct SimpleBencodeParser {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    SimpleBencodeParser(const uint8_t* d, size_t s) : data(d), size(s) {}

    std::string parseString() {
        size_t colon = pos;
        while (colon < size && data[colon] != ':') colon++;
        if (colon >= size) return "";
        std::string lenStr(reinterpret_cast<const char*>(data + pos), colon - pos);
        size_t len = std::stoull(lenStr);
        pos = colon + 1;
        if (pos + len > size) return "";
        std::string s(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return s;
    }

    int64_t parseInt() {
        pos++; // skip 'i'
        size_t end = pos;
        while (end < size && data[end] != 'e') end++;
        if (end >= size) return 0;
        std::string numStr(reinterpret_cast<const char*>(data + pos), end - pos);
        pos = end + 1;
        return std::stoll(numStr);
    }

    void skip() {
        if (pos >= size) return;
        char c = data[pos];
        if (c == 'i') {
            parseInt();
        } else if (c == 'l' || c == 'd') {
            pos++;
            while (pos < size && data[pos] != 'e') {
                skip();
            }
            if (pos < size && data[pos] == 'e') pos++;
        } else if (std::isdigit(c)) {
            parseString();
        } else {
            pos++;
        }
    }
};

static bool loadTorrentFilesFromMeta(const std::string& meta_path,
                                    std::string& out_root_name,
                                    std::vector<torrent::TorrentFileInfo>& out_files) {
    std::ifstream f(meta_path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.empty()) return false;

    // Fast-path: parse info dict
    SimpleBencodeParser p(buf.data(), buf.size());
    if (buf[0] != 'd') return false;
    p.pos = 1;

    // Find "files" or single file info
    std::string root_name;
    bool has_files = false;

    while (p.pos < p.size && p.data[p.pos] != 'e') {
        std::string key = p.parseString();
        if (key == "name") {
            root_name = p.parseString();
        } else if (key == "files") {
            has_files = true;
            if (p.data[p.pos] != 'l') break;
            p.pos++; // skip 'l'
            int file_idx = 0;
            while (p.pos < p.size && p.data[p.pos] != 'e') {
                if (p.data[p.pos] != 'd') break;
                p.pos++; // skip 'd'
                int64_t len = 0;
                std::string path;
                while (p.pos < p.size && p.data[p.pos] != 'e') {
                    std::string fkey = p.parseString();
                    if (fkey == "length") {
                        len = p.parseInt();
                    } else if (fkey == "path") {
                        if (p.data[p.pos] != 'l') break;
                        p.pos++;
                        while (p.pos < p.size && p.data[p.pos] != 'e') {
                            std::string part = p.parseString();
                            if (!path.empty()) path += "/";
                            path += part;
                        }
                        if (p.pos < p.size && p.data[p.pos] == 'e') p.pos++;
                    } else {
                        p.skip();
                    }
                }
                if (p.pos < p.size && p.data[p.pos] == 'e') p.pos++;
                
                torrent::TorrentFileInfo info;
                info.index = file_idx++;
                info.name = path;
                info.size = len;
                out_files.push_back(info);
            }
            if (p.pos < p.size && p.data[p.pos] == 'e') p.pos++;
        } else if (key == "length" && !has_files) {
            int64_t len = p.parseInt();
            torrent::TorrentFileInfo info;
            info.index = 0;
            info.name = root_name;
            info.size = len;
            out_files.push_back(info);
        } else {
            p.skip();
        }
    }
    out_root_name = root_name;
    return !out_files.empty();
}

int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << "    REAL USER MAGNETS VERIFICATION & HEURISTICS TEST    " << std::endl;
    std::cout << "========================================================" << std::endl;

    struct TestCase {
        std::string meta_path;
        std::string release_title;
        std::string expected_game_folder;
        bool has_nsp;
        bool has_archive;
    };

    std::vector<TestCase> cases = {
        {
            "cache/meta/8c6c01329b590abf9576986ab36d6dabef3d1ff7.meta",
            "[Nintendo Switch] Halo Combat Evolved (порт OpenCE) [NRO][ENG,RUS (Voice.)]",
            "halo",
            true,
            false
        },
        {
            "cache/meta/e5509845ec2b586170238751177af702dd0d428e.meta",
            "[Nintendo Switch] Flatout 2 (хоумбрю) [NRO][ENG]",
            "fo2x",
            true,
            false
        },
        {
            "cache/meta/f24283dd0a331489607780138b5a5c5bffb405d0.meta",
            "[Nintendo Switch] Silent Hill 2 EE (порт) [NRO][RUS]",
            "sh2-nx",
            true,
            true
        },
        {
            "cache/meta/73dabdbfc9123f9334110b4c06fb20512cca4c0e.meta",
            "[Nintendo Switch] Call of Duty Black Ops Zombies (iOS-порт) [NRO][ENG]",
            "boz",
            true,
            false
        }
    };

    auto& planner = installer::PortInstallPlanner::instance();

    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& tc = cases[i];
        std::cout << "\n--------------------------------------------------------" << std::endl;
        std::cout << "MAGNET " << (i + 1) << ": " << tc.release_title << std::endl;
        std::cout << "Meta file: " << tc.meta_path << std::endl;

        std::string root_name;
        std::vector<torrent::TorrentFileInfo> files;
        if (!loadTorrentFilesFromMeta(tc.meta_path, root_name, files)) {
            std::cerr << "FAIL: Failed to load meta from " << tc.meta_path << std::endl;
            return 1;
        }

        std::cout << "Total files in torrent: " << files.size() << std::endl;

        auto plan = planner.analyze(tc.release_title, files, "ru");

        std::cout << "  is_port:           " << (plan.is_port ? "TRUE" : "FALSE") << std::endl;
        std::cout << "  port_game_name:    " << plan.port_game_name << " (expected: " << tc.expected_game_folder << ")" << std::endl;
        std::cout << "  forwarder_nsp:     " << (plan.forwarder_nsp_index >= 0 ? plan.forwarder_nsp_name : "NONE") << std::endl;
        std::cout << "  variants detected: " << plan.variants.size() << std::endl;
        for (const auto& v : plan.variants) {
            std::cout << "    - Variant [" << v.id << "]: '" << v.title << "' (" << v.file_indices.size() << " files)" << std::endl;
        }
        std::cout << "  archives to extract: " << plan.archives_to_extract.size() << std::endl;
        for (const auto& a : plan.archives_to_extract) {
            std::cout << "    - Archive: " << a.archive_torrent_path << " -> " << a.extract_dest_dir << std::endl;
        }
        std::cout << "  overlay files:     " << plan.overlay_file_indices.size() << std::endl;
        for (int idx : plan.overlay_file_indices) {
            std::cout << "    - Overlay [" << idx << "]: " << files[idx].name
                      << " -> " << plan.file_destinations[idx] << std::endl;
        }

        // Assertions
        if (!plan.is_port) {
            std::cerr << "FAIL: Torrent was not detected as port!" << std::endl;
            return 1;
        }
        if (tc.has_nsp && plan.forwarder_nsp_index < 0) {
            std::cerr << "FAIL: Expected forwarder NSP, but none detected!" << std::endl;
            return 1;
        }
        if (tc.has_archive && plan.archives_to_extract.empty()) {
            std::cerr << "FAIL: Expected archive to extract, but none detected!" << std::endl;
            return 1;
        }
        if (plan.port_game_name != tc.expected_game_folder) {
            std::cerr << "FAIL: Port game folder mismatch! Got '" << plan.port_game_name
                      << "', expected '" << tc.expected_game_folder << "'" << std::endl;
            return 1;
        }

        // Check path remapping
        std::cout << "  Sample destination mappings:" << std::endl;
        int shown = 0;
        for (const auto& f : files) {
            if (f.index == plan.forwarder_nsp_index) continue;
            auto it = plan.file_destinations.find(f.index);
            if (it != plan.file_destinations.end() && shown < 4) {
                std::cout << "    * " << f.name << "  ==>  " << it->second << std::endl;
                shown++;
            }
        }

        std::cout << "  -> CHECK PASSED OK!" << std::endl;
    }

    std::cout << "\n========================================================" << std::endl;
    std::cout << "   ALL 4 REAL USER MAGNETS VERIFIED AND PASSED 100%!    " << std::endl;
    std::cout << "========================================================" << std::endl;
    return 0;
}
