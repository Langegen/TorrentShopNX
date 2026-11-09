#pragma once

// SHA-256 and MD5 without external libraries: devkitPro ships mbedtls 2.28
// and the PC build 3.6, whose APIs differ, and pctest links neither.

#include <cstddef>
#include <cstdint>
#include <string>

namespace util {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::string hexDigest();  // finishes; lower-case hex

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

class Md5 {
public:
    Md5();
    void update(const void* data, size_t len);
    std::string hexDigest();

private:
    void block(const uint8_t* p);
    uint32_t h_[4];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

// Both digests of a file in one pass. False if it cannot be read.
bool hashFile(const std::string& path, std::string* sha256_hex, std::string* md5_hex);

}  // namespace util
