#pragma once

#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <cstddef>

#ifdef __SWITCH__
#include <zstd.h>
extern "C" {
#include <switch/crypto/aes_ctr.h>
}
#endif

#include <functional>

namespace installer {

struct NcaHeader {
    uint8_t fixed_key_sig[0x100];
    uint8_t npdm_key_sig[0x100];
    uint32_t magic;
    uint8_t distribution;
    uint8_t content_type;
    uint8_t crypto_type;
    uint8_t kaek_index;
    uint64_t nca_size;
    uint64_t title_id;          // offset 0x210 (ProgramId)
    uint32_t content_index;     // offset 0x218
    uint32_t sdk_addon_version; // offset 0x21C
    uint8_t rest[0xC00 - 0x220];
#ifdef __SWITCH__
} NX_PACKED;
#else
} __attribute__((packed));
#endif

static_assert(sizeof(NcaHeader) == 0xC00, "NcaHeader must be 0xC00 bytes");

bool deriveNcaHeaderKey(uint8_t out_key[0x20]);
bool decryptNcaHeader(const uint8_t header_bytes[0x4000], const uint8_t header_key[0x20], NcaHeader& out_header);

class NczDecompressor {
public:
    using FetchCallback = std::function<size_t(void* buf, size_t size)>;

    // Инициализация декомпрессора, в которую передается функция чтения сжатых данных
    NczDecompressor(FetchCallback fetch_cb);
    ~NczDecompressor();

    bool init();
    
    // Читает декомпрессированные (и при необходимости зашифрованные AES-CTR) данные,
    // размером до size, возвращает сколько реально считано, либо 0 если конец.
    size_t read(void* buffer, size_t size);

    uint64_t getDecompressedSize() const { return decompressed_size_; }

private:
    uint64_t decompressed_size_ = 0;
#ifdef __SWITCH__
    FetchCallback fetch_cb_;
    
    struct NczSection {
        uint64_t offset;
        uint64_t size;
        uint64_t crypto_type;
        uint64_t padding;
        uint8_t crypto_key[16];
        uint8_t crypto_counter[16];
    };
    std::vector<NczSection> sections_;

    bool use_block_compression_ = false;
    bool failed_ = false;
    uint64_t decompressed_body_size_ = 0;
    
    // Zstd state
    ZSTD_DCtx* zstd_dctx_ = nullptr;

    // Stream buffering for input compressed data
    size_t fetchInput(void* buffer, size_t size);
    
    // Внутренний буфер для NCA хедера и распаковки
    uint8_t nca_header_[0x4000];
    std::vector<uint8_t> block_decomp_buf_;
    size_t block_decomp_off_ = 0;
    
    // Состояние потока выдачи
    uint64_t current_output_offset_ = 0;
    
    // Буфер, который хранит расжатые данные (Zstd -> out)
    ZSTD_inBuffer zstd_in_ = {nullptr, 0, 0};
    std::vector<uint8_t> compressed_input_buf_;

    // Если block compression = true
    uint32_t block_size_ = 0;
    std::vector<uint32_t> compressed_block_sizes_;
    size_t current_block_id_ = 0;
    std::vector<uint8_t> compressed_block_buf_;
    
    // Текущая секция для AES-CTR
    void applyAesCtrIfNeed(void* buf, size_t size, uint64_t global_offset);
    void seekAesCtr(uint64_t offset, const NczSection& sec, unsigned char nonce_counter[16], size_t& nc_off);

    bool ctr_initialized_ = false;
    uint64_t ctr_next_offset_ = 0;
    const NczSection* ctr_current_sec_ = nullptr;
    Aes128CtrContext ctr_ctx_;

#endif // __SWITCH__
};

} // namespace installer
