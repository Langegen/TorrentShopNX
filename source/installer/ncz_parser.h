#pragma once

#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <cstddef>

#ifdef __SWITCH__
#include <zstd.h>
#include <mbedtls/aes.h>
#endif

#include <functional>

namespace installer {

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
    uint64_t decompressed_size_ = 0;
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
    void seekAesCtr(uint64_t offset, const NczSection& sec, mbedtls_aes_context& aes, unsigned char nonce_counter[16], size_t& nc_off);

#endif // __SWITCH__
};

} // namespace installer
