#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace installer {

class StreamInstaller {
public:
    explicit StreamInstaller(size_t buffer_size_bytes = 64 * 1024 * 1024);

    bool openStream(const std::string& name);
    size_t readChunk(const void* data, size_t size);
    bool installChunk();
    bool isComplete() const { return completed_; }
    unsigned long long totalSize() const { return total_size_; }
    unsigned long long writtenSize() const { return written_size_; }

private:
    void processBuffer();
    bool parseHeader();
    bool parseEntriesAndNames();
    void closeCurrentFile();
    bool ensureOutputDir();
    std::string sanitizeName(const std::string& name) const;

    struct PfsEntry {
        unsigned long long offset = 0;
        unsigned long long size = 0;
        unsigned int name_offset = 0;
        std::string name;
    };

    std::vector<unsigned char> buffer_;
    size_t buffer_capacity_ = 0;
    size_t stream_pos_ = 0;
    bool header_parsed_ = false;
    bool entries_parsed_ = false;
    bool completed_ = false;
    unsigned int file_count_ = 0;
    unsigned int string_table_size_ = 0;
    size_t data_region_start_ = 0;
    std::vector<PfsEntry> entries_;
    size_t current_file_index_ = 0;
    size_t current_file_written_ = 0;
    unsigned long long total_size_ = 0;
    unsigned long long written_size_ = 0;
    std::vector<void*> file_handles_;
    std::string stream_name_;
    std::string output_dir_;
};

} // namespace installer
