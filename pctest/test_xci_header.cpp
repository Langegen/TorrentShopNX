// XciHeader::parse on crafted in-memory images: valid layout and hostile HFS0 counts.
// Build: g++ -std=c++17 -Isource -Iinclude pctest/test_xci_header.cpp source/installer/xci_header.cpp -o pctest/test_xci_header
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../source/installer/xci_header.h"

namespace util {
void logLine(const std::string&) {}
}

namespace {

class MemSource : public datasource::IDataSource {
public:
    explicit MemSource(std::vector<uint8_t> d) : data_(std::move(d)) {}
    bool open(const std::string&, int) override { return true; }
    size_t read(uint64_t off, void* buf, size_t size) override {
        if (off >= data_.size()) return 0;
        size_t n = std::min<uint64_t>(size, data_.size() - off);
        std::memcpy(buf, data_.data() + off, n);
        return n;
    }
    uint64_t totalSize() const override { return data_.size(); }
    bool isAvailable() const override { return true; }
    datasource::SourceType type() const override { return datasource::SourceType::File; }
    void close() override {}

private:
    std::vector<uint8_t> data_;
};

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
void put64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
    for (int i = 0; i < 8; i++) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

// One-entry HFS0 at `at`; returns its total header size.
size_t putHfs0(std::vector<uint8_t>& b, size_t at, const char* name, uint64_t off, uint64_t size) {
    const uint32_t strtab = 0x40;
    std::memcpy(&b[at], "HFS0", 4);
    put32(b, at + 4, 1);
    put32(b, at + 8, strtab);
    put64(b, at + 0x10, off);
    put64(b, at + 0x18, size);
    put32(b, at + 0x20, 0);
    std::strcpy(reinterpret_cast<char*>(&b[at + 0x10 + 0x40]), name);
    return 0x10 + 0x40 + strtab;
}

// HEAD at 0x100, root HFS0 at 0x200 with "secure", secure HFS0 with one ticket.
std::vector<uint8_t> validXci() {
    std::vector<uint8_t> b(0x1000, 0);
    std::memcpy(&b[0x100], "HEAD", 4);
    put64(b, 0x138, 0x200);
    size_t root = putHfs0(b, 0x200, "secure", 0, 0x200);
    putHfs0(b, 0x200 + root, "0123456789abcdef0123456789abcdef.tik", 0, 0x40);
    return b;
}

bool parse(std::vector<uint8_t> img) {
    MemSource src(img);
    installer::XciHeader h;
    return h.parse(&src, img.size());
}

}  // namespace

int main() {
    {
        auto img = validXci();
        MemSource src(img);
        installer::XciHeader h;
        assert(h.parse(&src, img.size()));
        assert(h.fileCount() == 1);
        assert(h.hasTicket());
        std::puts("valid_xci ... OK");
    }
    {
        // 0x04000000 * 0x40 wraps to 0 in 32-bit math.
        auto img = validXci();
        put32(img, 0x204, 0x04000000);
        assert(!parse(img));
        std::puts("file_count_overflow_rejected ... OK");
    }
    {
        auto img = validXci();
        put32(img, 0x208, 0xFFFFFFF0u);
        assert(!parse(img));
        std::puts("huge_string_table_rejected ... OK");
    }
    std::puts("all xci_header tests passed");
    return 0;
}
