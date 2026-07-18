#include "ncz_parser.h"

#ifdef __SWITCH__
#include "../buffer/ring_buffer.h"
#include "../utils/log.h"
#include <algorithm>
#include <cstring>

namespace installer {

namespace {

constexpr uint32_t kNca3Magic = 0x3341434E; // "NCA3"
constexpr uint8_t kHeaderKekSource[0x10] = {
    0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D,
    0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A
};
constexpr uint8_t kHeaderKeySource[0x20] = {
    0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26,
    0x31, 0xF7, 0xE2, 0x5D, 0x19, 0x7B, 0xF5, 0xD0,
    0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28, 0x18, 0x3D,
    0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2
};

struct NcaHeader {
    uint8_t fixed_key_sig[0x100];
    uint8_t npdm_key_sig[0x100];
    uint32_t magic;
    uint8_t distribution;
    uint8_t content_type;
    uint8_t crypto_type;
    uint8_t kaek_index;
    uint64_t nca_size;
    uint8_t rest[0xC00 - 0x210];
} NX_PACKED;

static_assert(sizeof(NcaHeader) == 0xC00, "NcaHeader must be 0xC00 bytes");

bool deriveNcaHeaderKey(uint8_t out_key[0x20]) {
    Result rc = splCryptoInitialize();
    if (R_FAILED(rc)) {
        util::logLine("ncz: splCryptoInitialize failed, rc=" + std::to_string(rc));
        return false;
    }

    uint8_t sealed_kek[0x10] = {};
    rc = splCryptoGenerateAesKek(kHeaderKekSource, 0, 0, sealed_kek);
    if (R_SUCCEEDED(rc)) {
        rc = splCryptoGenerateAesKey(sealed_kek, kHeaderKeySource, out_key);
    }
    if (R_SUCCEEDED(rc)) {
        rc = splCryptoGenerateAesKey(sealed_kek, kHeaderKeySource + 0x10, out_key + 0x10);
    }

    splCryptoExit();

    if (R_FAILED(rc)) {
        util::logLine("ncz: failed to derive NCA header key, rc=" + std::to_string(rc));
        return false;
    }

    return true;
}

bool decryptNcaHeader(const uint8_t header_bytes[0x4000], const uint8_t header_key[0x20], NcaHeader& out_header) {
    Aes128XtsContext xts;
    aes128XtsContextCreate(&xts, header_key, header_key + 0x10, false);
    for (size_t offset = 0; offset < sizeof(out_header); offset += 0x200) {
        aes128XtsContextResetSector(&xts, offset / 0x200, true);
        aes128XtsDecrypt(&xts,
                         reinterpret_cast<uint8_t*>(&out_header) + offset,
                         header_bytes + offset,
                         0x200);
    }

    if (out_header.magic != kNca3Magic || out_header.nca_size < sizeof(NcaHeader)) {
        util::logLine("ncz: failed to decode NCA header size");
        return false;
    }

    return true;
}

void encryptNcaHeader(const NcaHeader& header, const uint8_t header_key[0x20], uint8_t header_bytes[0x4000]) {
    Aes128XtsContext xts;
    aes128XtsContextCreate(&xts, header_key, header_key + 0x10, true);
    for (size_t offset = 0; offset < sizeof(header); offset += 0x200) {
        aes128XtsContextResetSector(&xts, offset / 0x200, true);
        aes128XtsEncrypt(&xts,
                         header_bytes + offset,
                         reinterpret_cast<const uint8_t*>(&header) + offset,
                         0x200);
    }
}

void incrementCtrCounter(unsigned char nonce_counter[16]) {
    for (int i = 15; i >= 8; --i) {
        ++nonce_counter[i];
        if (nonce_counter[i] != 0) {
            break;
        }
    }
}

} // namespace

NczDecompressor::NczDecompressor(FetchCallback fetch_cb)
    : fetch_cb_(fetch_cb) {
}

NczDecompressor::~NczDecompressor() {
    if (zstd_dctx_) {
        ZSTD_freeDCtx(zstd_dctx_);
    }
}

size_t NczDecompressor::fetchInput(void* buffer, size_t size) {
    if (fetch_cb_) {
        uint8_t* ptr = static_cast<uint8_t*>(buffer);
        size_t total_read = 0;
        while (total_read < size) {
            size_t r = fetch_cb_(ptr + total_read, size - total_read);
            if (r == 0) break; // EOF
            total_read += r;
        }
        return total_read;
    }
    return 0;
}

bool NczDecompressor::init() {
    sections_.clear();
    compressed_block_sizes_.clear();
    compressed_input_buf_.clear();
    block_decomp_buf_.clear();
    use_block_compression_ = false;
    failed_ = false;
    decompressed_size_ = sizeof(nca_header_);
    decompressed_body_size_ = 0;

    const auto read_exact = [this](void* dst, size_t len, const char* what) -> bool {
        if (fetchInput(dst, len) != len) {
            util::logLine(std::string("ncz: failed to read ") + what);
            return false;
        }
        return true;
    };

    if (!read_exact(nca_header_, sizeof(nca_header_), "16KB nca header")) {
        return false;
    }

    uint8_t header_key[0x20] = {};
    NcaHeader header = {};
    uint64_t header_nca_size = 0;
    bool have_header_nca_size = false;
    if (deriveNcaHeaderKey(header_key) && decryptNcaHeader(nca_header_, header_key, header)) {
        header_nca_size = header.nca_size;
        have_header_nca_size = true;

        if (header.distribution == 1) {
            header.distribution = 0;
            encryptNcaHeader(header, header_key, nca_header_);
            util::logLine("ncz: normalized NCA header distribution for install");
        }
    }

    char magic[8];
    if (!read_exact(magic, sizeof(magic), "NCZSECTN magic") || std::memcmp(magic, "NCZSECTN", 8) != 0) {
        util::logLine("ncz: invalid NCZSECTN magic");
        return false;
    }

    uint64_t section_count = 0;
    if (!read_exact(&section_count, sizeof(section_count), "section count")) {
        return false;
    }

    std::vector<NczSection> raw_sections;
    raw_sections.reserve(static_cast<size_t>(section_count));

    for (uint64_t i = 0; i < section_count; ++i) {
        NczSection sec = {};
        if (!read_exact(&sec.offset, sizeof(sec.offset), "section offset") ||
            !read_exact(&sec.size, sizeof(sec.size), "section size") ||
            !read_exact(&sec.crypto_type, sizeof(sec.crypto_type), "section crypto type") ||
            !read_exact(&sec.padding, sizeof(sec.padding), "section padding") ||
            !read_exact(sec.crypto_key, sizeof(sec.crypto_key), "section crypto key") ||
            !read_exact(sec.crypto_counter, sizeof(sec.crypto_counter), "section crypto counter")) {
            return false;
        }
        raw_sections.push_back(sec);
    }

    std::sort(raw_sections.begin(), raw_sections.end(), [](const NczSection& a, const NczSection& b) {
        return a.offset < b.offset;
    });

    uint64_t max_section_end = sizeof(nca_header_);
    uint64_t next_expected_offset = sizeof(nca_header_);
    for (const auto& sec : raw_sections) {
        if (sec.offset > next_expected_offset) {
            NczSection gap = {};
            gap.offset = next_expected_offset;
            gap.size = sec.offset - next_expected_offset;
            sections_.push_back(gap);
        } else if (sec.offset < next_expected_offset) {
            util::logLine("ncz: overlapping or unsorted section at offset=" + std::to_string(sec.offset));
        }

        sections_.push_back(sec);
        const uint64_t sec_end = sec.offset + sec.size;
        max_section_end = std::max(max_section_end, sec_end);
        next_expected_offset = std::max(next_expected_offset, sec_end);
    }

    uint64_t block_body_size = 0;
    if (!read_exact(magic, sizeof(magic), "stream or NCZBLOCK magic")) {
        return false;
    }

    if (std::memcmp(magic, "NCZBLOCK", 8) == 0) {
        use_block_compression_ = true;
        uint8_t version = 0;
        uint8_t type = 0;
        uint8_t unused = 0;
        uint8_t block_size_exp = 0;

        if (!read_exact(&version, sizeof(version), "NCZBLOCK version") ||
            !read_exact(&type, sizeof(type), "NCZBLOCK type") ||
            !read_exact(&unused, sizeof(unused), "NCZBLOCK unused") ||
            !read_exact(&block_size_exp, sizeof(block_size_exp), "NCZBLOCK block size exponent")) {
            return false;
        }

        uint32_t num_blocks = 0;
        if (!read_exact(&num_blocks, sizeof(num_blocks), "NCZBLOCK block count") ||
            !read_exact(&block_body_size, sizeof(block_body_size), "NCZBLOCK decompressed size")) {
            return false;
        }

        block_size_ = 1u << block_size_exp;
        compressed_block_sizes_.resize(num_blocks);
        for (uint32_t i = 0; i < num_blocks; ++i) {
            if (!read_exact(&compressed_block_sizes_[i], sizeof(compressed_block_sizes_[i]), "NCZBLOCK compressed block size")) {
                return false;
            }
        }
        util::logLine("ncz: using NCZBLOCK compression (" + std::to_string(num_blocks) + " blocks, size 2^" + std::to_string(block_size_exp) + ")");
    } else {
        compressed_input_buf_.insert(compressed_input_buf_.end(),
                                     reinterpret_cast<const uint8_t*>(magic),
                                     reinterpret_cast<const uint8_t*>(magic) + sizeof(magic));

        zstd_in_.src = compressed_input_buf_.data();
        zstd_in_.size = compressed_input_buf_.size();
        zstd_in_.pos = 0;

        util::logLine("ncz: using Solid Zstd decompression");
    }

    uint64_t resolved_nca_size = max_section_end;
    if (have_header_nca_size) {
        resolved_nca_size = std::max(resolved_nca_size, header_nca_size);
    }
    if (block_body_size > 0) {
        resolved_nca_size = std::max(resolved_nca_size, sizeof(nca_header_) + block_body_size);
    }
    if (resolved_nca_size < sizeof(nca_header_)) {
        util::logLine("ncz: invalid resolved NCA size");
        return false;
    }

    decompressed_size_ = resolved_nca_size;
    decompressed_body_size_ = decompressed_size_ - sizeof(nca_header_);

    if (have_header_nca_size && header_nca_size != resolved_nca_size) {
        util::logLine("ncz: NCA size mismatch, header=" + std::to_string(header_nca_size)
                      + " resolved=" + std::to_string(resolved_nca_size)
                      + " max_section_end=" + std::to_string(max_section_end));
    }

    zstd_dctx_ = ZSTD_createDCtx();
    if (!zstd_dctx_) {
        return false;
    }

    current_output_offset_ = 0;
    current_block_id_ = 0;
    block_decomp_off_ = 0;

    return true;
}

void NczDecompressor::seekAesCtr(uint64_t offset, const NczSection& sec, mbedtls_aes_context& aes, unsigned char nonce_counter[16], size_t& nc_off) {
    mbedtls_aes_setkey_enc(&aes, sec.crypto_key, 128);
    
    // Construct counter from nonce & offset
    std::memcpy(nonce_counter, sec.crypto_counter, 8);
    uint64_t off_val = offset >> 4;
    // big endian write of off_val into the last 8 bytes of the counter
    for (int i = 7; i >= 0; --i) {
        nonce_counter[8 + i] = static_cast<unsigned char>(off_val & 0xFF);
        off_val >>= 8;
    }
    
    nc_off = offset & 0xF;
}

void NczDecompressor::applyAesCtrIfNeed(void* buf, size_t size, uint64_t global_offset) {
    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t remaining = size;
    uint64_t current_off = global_offset;

    while (remaining > 0) {
        // Найти секцию
        const NczSection* active_sec = nullptr;
        uint64_t next_boundary = current_off + remaining;
        for (const auto& sec : sections_) {
            const uint64_t sec_start = sec.offset;
            const uint64_t sec_end = sec.offset + sec.size;
            if (current_off >= sec_start && current_off < sec_end) {
                active_sec = &sec;
                next_boundary = sec_end;
                break;
            }
            if (sec_start > current_off && sec_start < next_boundary) {
                next_boundary = sec_start;
            }
        }

        size_t chunk_size = static_cast<size_t>(std::min<uint64_t>(remaining, next_boundary - current_off));
        if (chunk_size == 0) {
            break;
        }
        if (active_sec) {
            if (active_sec->crypto_type == 3 || active_sec->crypto_type == 4) {
                mbedtls_aes_context aes;
                mbedtls_aes_init(&aes);
                
                unsigned char nonce_counter[16] = {0};
                unsigned char stream_block[16] = {0};
                size_t nc_off = 0;
                
                seekAesCtr(current_off, *active_sec, aes, nonce_counter, nc_off);
                
                // If nc_off > 0, we must prep the stream_block by encrypting the nonce_counter once
                if (nc_off > 0) {
                    unsigned char tmp_nonce[16];
                    std::memcpy(tmp_nonce, nonce_counter, 16);
                    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, tmp_nonce, stream_block);
                    incrementCtrCounter(nonce_counter);
                }
                
                mbedtls_aes_crypt_ctr(&aes, chunk_size, &nc_off, nonce_counter, stream_block, ptr, ptr);
                
                mbedtls_aes_free(&aes);
            }
        }
        ptr += chunk_size;
        current_off += chunk_size;
        remaining -= chunk_size;
    }
}

size_t NczDecompressor::read(void* buffer, size_t size) {
    if (failed_) {
        return 0;
    }

    uint8_t* out_ptr = static_cast<uint8_t*>(buffer);
    size_t total_written = 0;

    while (total_written < size && current_output_offset_ < decompressed_size_) {
        // 1. NCA Header (0x4000)
        if (current_output_offset_ < 0x4000) {
            size_t chunk = size - total_written;
            if (current_output_offset_ + chunk > 0x4000) {
                chunk = 0x4000 - current_output_offset_;
            }
            memcpy(out_ptr + total_written, nca_header_ + current_output_offset_, chunk);
            total_written += chunk;
            current_output_offset_ += chunk;
            continue;
        }

        // 2. Декомпрессия
        size_t needed = size - total_written;
        
        if (use_block_compression_) {
            // Если в буфере пусто, читаем следующий блок
            if (block_decomp_off_ >= block_decomp_buf_.size()) {
                if (current_block_id_ >= compressed_block_sizes_.size()) {
                    break; // EOF
                }
                
                uint32_t comp_size = compressed_block_sizes_[current_block_id_++];
                
                // Calculate expected decompressed size for this block
                // but since we read blocks sequentially, current_block_id_ * block_size_ is the offset in compressed area.
                uint64_t offset_in_comp_area = (uint64_t)(current_block_id_ - 1) * block_size_;
                uint64_t comp_area_total_size = decompressed_body_size_;
                
                uint32_t expected_decomp_size = block_size_;
                if (offset_in_comp_area + block_size_ > comp_area_total_size) {
                    expected_decomp_size = static_cast<uint32_t>(comp_area_total_size - offset_in_comp_area);
                }

                if (compressed_block_buf_.size() < comp_size) {
                    compressed_block_buf_.resize(comp_size);
                }
                if (fetchInput(compressed_block_buf_.data(), comp_size) != comp_size) {
                    util::logLine("ncz: failed to fetch compressed block " + std::to_string(current_block_id_ - 1));
                    failed_ = true;
                    break;
                }
                
                if (comp_size >= expected_decomp_size) {
                    // STORED block (uncompressed)
                    block_decomp_buf_.resize(comp_size);
                    std::memcpy(block_decomp_buf_.data(), compressed_block_buf_.data(), comp_size);
                } else {
                    // COMPRESSED block
                    block_decomp_buf_.resize(expected_decomp_size);
                    size_t dSize = ZSTD_decompressDCtx(zstd_dctx_, block_decomp_buf_.data(), expected_decomp_size, compressed_block_buf_.data(), comp_size);
                    if (ZSTD_isError(dSize)) {
                        util::logLine(std::string("ncz: zstd err in block ") + std::to_string(current_block_id_-1) + ": " + ZSTD_getErrorName(dSize));
                        failed_ = true;
                        break;
                    }
                    block_decomp_buf_.resize(dSize);
                }
                block_decomp_off_ = 0;
            }
            
            size_t avail = block_decomp_buf_.size() - block_decomp_off_;
            size_t chunk = (needed < avail) ? needed : avail;
            
            uint64_t goff = current_output_offset_;
            memcpy(out_ptr + total_written, block_decomp_buf_.data() + block_decomp_off_, chunk);
            applyAesCtrIfNeed(out_ptr + total_written, chunk, goff);
            
            block_decomp_off_ += chunk;
            total_written += chunk;
            current_output_offset_ += chunk;

        } else {
            // Режим сплошного потока (Solid)
            ZSTD_outBuffer out = { out_ptr + total_written, needed, 0 };
            
            while (out.pos < out.size) {
                if (zstd_in_.pos >= zstd_in_.size) {
                    // Buffer is empty, fetch more raw data
                    if (compressed_input_buf_.size() < 512 * 1024) {
                        compressed_input_buf_.resize(512 * 1024);
                    }
                    size_t res = fetchInput(compressed_input_buf_.data(), compressed_input_buf_.size());
                    if (res == 0) break; // EOF
                    zstd_in_.src = compressed_input_buf_.data();
                    zstd_in_.size = res;
                    zstd_in_.pos = 0;
                }
                
                size_t old_out_pos = out.pos;
                size_t dRes = ZSTD_decompressStream(zstd_dctx_, &out, &zstd_in_);
                if (ZSTD_isError(dRes)) {
                    util::logLine(std::string("ncz: zstd solid err: ") + ZSTD_getErrorName(dRes));
                    failed_ = true;
                    break;
                }

                // If we got some data, apply AES
                size_t decoded = out.pos - old_out_pos;
                if (decoded > 0) {
                    applyAesCtrIfNeed(out_ptr + total_written + old_out_pos, decoded, current_output_offset_);
                    current_output_offset_ += decoded;
                }
                
                if (decoded == 0 && dRes == 0) {
                    break;
                }
            }
            
            total_written += out.pos;
            if (failed_ || (out.pos == 0 && zstd_in_.pos >= zstd_in_.size)) break; // fatal error or true EOF
        }
    }
    
    return total_written;
}

} // namespace installer
#endif
