#include "stream_installer.h"

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <sys/stat.h>

#include "../utils/app_paths.h"

namespace installer {

static unsigned int readU32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned long long readU64(const unsigned char* p) {
    return (unsigned long long)p[0] | ((unsigned long long)p[1] << 8) | ((unsigned long long)p[2] << 16) |
           ((unsigned long long)p[3] << 24) | ((unsigned long long)p[4] << 32) | ((unsigned long long)p[5] << 40) |
           ((unsigned long long)p[6] << 48) | ((unsigned long long)p[7] << 56);
}

static bool pathExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool ensureDirRecursive(const std::string& path) {
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

StreamInstaller::StreamInstaller(size_t buffer_size_bytes)
    : buffer_capacity_(buffer_size_bytes) {
    buffer_.reserve(buffer_capacity_);
}

bool StreamInstaller::openStream(const std::string& name) {
    stream_name_ = name;
    output_dir_ = std::string(TSNX_CACHE_STREAM) + "/" + stream_name_;
    buffer_.clear();
    stream_pos_ = 0;
    header_parsed_ = false;
    entries_parsed_ = false;
    completed_ = false;
    file_count_ = 0;
    string_table_size_ = 0;
    data_region_start_ = 0;
    total_size_ = 0;
    written_size_ = 0;
    entries_.clear();
    current_file_index_ = 0;
    current_file_written_ = 0;
    for (void* f : file_handles_) {
        if (f) std::fclose(static_cast<FILE*>(f));
    }
    file_handles_.clear();
    return true;
}

bool StreamInstaller::ensureOutputDir() {
    return ensureDirRecursive(output_dir_);
}

std::string StreamInstaller::sanitizeName(const std::string& name) const {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "file";
    return out;
}

bool StreamInstaller::parseHeader() {
    if (header_parsed_) return true;
    if (stream_pos_ != 0) return false;
    if (buffer_.size() < 0x10) return false;
    if (std::memcmp(buffer_.data(), "PFS0", 4) != 0) {
        return false;
    }
    file_count_ = readU32(buffer_.data() + 4);
    string_table_size_ = readU32(buffer_.data() + 8);
    data_region_start_ = 0x10 + (size_t)file_count_ * 0x18 + string_table_size_;
    header_parsed_ = true;
    return true;
}

bool StreamInstaller::parseEntriesAndNames() {
    if (!header_parsed_ || entries_parsed_) return entries_parsed_;
    if (stream_pos_ != 0) return false;
    if (buffer_.size() < data_region_start_) return false;

    size_t entry_table = 0x10;
    size_t string_table = 0x10 + (size_t)file_count_ * 0x18;

    entries_.clear();
    entries_.reserve(file_count_);

    for (unsigned int i = 0; i < file_count_; ++i) {
        size_t off = entry_table + i * 0x18;
        PfsEntry e;
        e.offset = readU64(buffer_.data() + off);
        e.size = readU64(buffer_.data() + off + 8);
        e.name_offset = readU32(buffer_.data() + off + 16);
        entries_.push_back(e);
    }
    total_size_ = 0;
    for (const auto& e : entries_) {
        total_size_ += e.size;
    }

    for (auto& e : entries_) {
        size_t name_pos = string_table + e.name_offset;
        if (name_pos >= buffer_.size()) {
            e.name = "file";
            continue;
        }
        std::string name;
        for (size_t i = name_pos; i < buffer_.size(); ++i) {
            char c = static_cast<char>(buffer_[i]);
            if (c == '\0') break;
            name.push_back(c);
        }
        e.name = sanitizeName(name);
    }

    if (!ensureOutputDir()) return false;
    file_handles_.clear();
    file_handles_.reserve(entries_.size());
    for (const auto& e : entries_) {
        std::string path = output_dir_ + "/" + e.name;
        FILE* f = std::fopen(path.c_str(), "wb");
        file_handles_.push_back(f);
    }

    entries_parsed_ = true;
    return true;
}

void StreamInstaller::closeCurrentFile() {
    if (current_file_index_ >= file_handles_.size()) return;
    FILE* f = static_cast<FILE*>(file_handles_[current_file_index_]);
    if (f) {
        std::fclose(f);
        file_handles_[current_file_index_] = nullptr;
    }
}

void StreamInstaller::processBuffer() {
    if (completed_) return;

    parseHeader();
    parseEntriesAndNames();

    while (!buffer_.empty()) {
        if (!entries_parsed_) break;

        size_t abs_pos = stream_pos_;
        if (abs_pos < data_region_start_) {
            size_t skip = std::min(buffer_.size(), data_region_start_ - abs_pos);
            buffer_.erase(buffer_.begin(), buffer_.begin() + skip);
            stream_pos_ += skip;
            continue;
        }

        if (current_file_index_ >= entries_.size()) {
            buffer_.clear();
            completed_ = true;
            break;
        }

        const auto& e = entries_[current_file_index_];
        size_t file_start = data_region_start_ + (size_t)e.offset + current_file_written_;

        if (abs_pos < file_start) {
            size_t skip = std::min(buffer_.size(), file_start - abs_pos);
            buffer_.erase(buffer_.begin(), buffer_.begin() + skip);
            stream_pos_ += skip;
            continue;
        }

        size_t remaining = (size_t)e.size - current_file_written_;
        if (remaining == 0) {
            closeCurrentFile();
            current_file_index_++;
            current_file_written_ = 0;
            if (current_file_index_ >= entries_.size()) {
                completed_ = true;
            }
            continue;
        }

        size_t to_write = std::min(buffer_.size(), remaining);
        FILE* f = static_cast<FILE*>(file_handles_[current_file_index_]);
        if (f && to_write > 0) {
            std::fwrite(buffer_.data(), 1, to_write, f);
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + to_write);
        stream_pos_ += to_write;
        current_file_written_ += to_write;
        written_size_ += to_write;

        if (current_file_written_ >= e.size) {
            closeCurrentFile();
            current_file_index_++;
            current_file_written_ = 0;
            if (current_file_index_ >= entries_.size()) {
                completed_ = true;
            }
        }
    }
}

size_t StreamInstaller::readChunk(const void* data, size_t size) {
    if (size == 0) return 0;
    size_t available = buffer_capacity_ - buffer_.size();
    size_t to_copy = size < available ? size : available;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    buffer_.insert(buffer_.end(), p, p + to_copy);
    processBuffer();
    return to_copy;
}

bool StreamInstaller::installChunk() {
    processBuffer();
    return completed_;
}

} // namespace installer
