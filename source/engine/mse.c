#include "mse.h"
#include <string.h>
#include <stdlib.h>
#include "mbedtls/sha1.h"

// 768-bit prime P for BitTorrent MSE (RFC 3526 MODP 1)
static const unsigned char s_dhm_P[96] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC9, 0x0F, 0xDA, 0xA2,
    0x21, 0x68, 0xC2, 0x34, 0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1,
    0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67, 0xCC, 0x74, 0x02, 0x0B, 0xBE, 0xA6,
    0x3B, 0x13, 0x9B, 0x22, 0x51, 0x4A, 0x08, 0x79, 0x8E, 0x34, 0x04, 0xDD,
    0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B, 0x30, 0x2B, 0x0A, 0x6D,
    0xF2, 0x5F, 0x14, 0x37, 0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45,
    0xE4, 0x85, 0xB5, 0x76, 0x62, 0x5E, 0x7E, 0xC6, 0xF4, 0x4C, 0x42, 0xE9,
    0xA6, 0x37, 0xED, 0x6B, 0x0B, 0xFF, 0x5C, 0xB6, 0xF4, 0x06, 0xB7, 0xED
};
static const unsigned char s_dhm_G[1] = { 0x02 };

static int mse_rng(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    for (size_t i = 0; i < len; i++) out[i] = (unsigned char)(rand() % 256);
    return 0;
}

static void rc4_init(mse_rc4 *rc4, const uint8_t *key, size_t keylen) {
    rc4->x = rc4->y = 0;
    for (int i = 0; i < 256; i++) rc4->state[i] = (uint8_t)i;
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = (uint8_t)(j + rc4->state[i] + key[i % keylen]);
        uint8_t tmp = rc4->state[i];
        rc4->state[i] = rc4->state[j];
        rc4->state[j] = tmp;
    }
}

static void rc4_crypt(mse_rc4 *rc4, uint8_t *data, size_t len) {
    uint8_t x = rc4->x, y = rc4->y;
    for (size_t i = 0; i < len; i++) {
        x = (uint8_t)(x + 1);
        y = (uint8_t)(y + rc4->state[x]);
        uint8_t tmp = rc4->state[x];
        rc4->state[x] = rc4->state[y];
        rc4->state[y] = tmp;
        data[i] ^= rc4->state[(uint8_t)(rc4->state[x] + rc4->state[y])];
    }
    rc4->x = x;
    rc4->y = y;
}

static void rc4_drop(mse_rc4 *rc4, size_t count) {
    uint8_t dummy[64] = {0};
    while (count > 0) {
        size_t n = count > sizeof(dummy) ? sizeof(dummy) : count;
        rc4_crypt(rc4, dummy, n);
        count -= n;
    }
}

static void sha1_hash(const char *prefix, size_t plen,
                      const uint8_t *s, size_t slen,
                      const uint8_t *skey, size_t skeylen,
                      uint8_t out[20]) {
    mbedtls_sha1_context sha;
    mbedtls_sha1_init(&sha);
    mbedtls_sha1_starts(&sha);
    if (prefix && plen > 0) mbedtls_sha1_update(&sha, (const unsigned char*)prefix, plen);
    if (s && slen > 0) mbedtls_sha1_update(&sha, s, slen);
    if (skey && skeylen > 0) mbedtls_sha1_update(&sha, skey, skeylen);
    mbedtls_sha1_finish(&sha, out);
    mbedtls_sha1_free(&sha);
}

void mse_init(mse_state *m, const uint8_t info_hash[20], const uint8_t peer_id[20]) {
    memset(m, 0, sizeof(*m));
    memcpy(m->info_hash, info_hash, 20);
    memcpy(m->peer_id, peer_id, 20);
    m->enabled = true;
    m->phase = MSE_STATE_IDLE;

    mbedtls_dhm_init(&m->dhm);
    mbedtls_mpi_read_binary(&m->dhm.P, s_dhm_P, 96);
    mbedtls_mpi_read_binary(&m->dhm.G, s_dhm_G, 1);
    m->dhm.len = 96;
}

void mse_free(mse_state *m) {
    mbedtls_dhm_free(&m->dhm);
    memset(m, 0, sizeof(*m));
}

int mse_create_ya(mse_state *m, uint8_t *out_buf, size_t max_len) {
    if (max_len < 96) return -1;
    if (mbedtls_dhm_make_public(&m->dhm, 20, m->ya, 96, mse_rng, NULL) != 0)
        return -1;
    memcpy(out_buf, m->ya, 96);
    m->phase = MSE_STATE_SENT_YA;
    return 96;
}

int mse_process_rx(mse_state *m,
                   uint8_t *rx, size_t *rx_len, size_t *rx_off,
                   uint8_t *out_tx, size_t *out_tx_len,
                   bool *is_plaintext_fallback) {
    *is_plaintext_fallback = false;
    size_t avail = *rx_len - *rx_off;

    // Check if peer is sending a legacy plaintext BitTorrent handshake
    if (avail >= 20 && rx[*rx_off] == 19 &&
        memcmp(rx + *rx_off + 1, "BitTorrent protocol", 19) == 0) {
        m->phase = MSE_STATE_PLAINTEXT;
        m->enabled = false;
        *is_plaintext_fallback = true;
        return 0;
    }

    if (m->phase == MSE_STATE_SENT_YA) {
        if (avail < 96) return 0; // wait for Yb (96 bytes)

        uint8_t yb[96];
        memcpy(yb, rx + *rx_off, 96);
        *rx_off += 96;

        if (mbedtls_dhm_read_public(&m->dhm, yb, 96) != 0) return -1;

        size_t olen = 0;
        if (mbedtls_dhm_calc_secret(&m->dhm, m->secret, 96, &olen, NULL, NULL) != 0)
            return -1;

        // Derive keys
        uint8_t req1[20], req2[20], req3[20], enchash[20];
        sha1_hash("req1", 4, m->secret, 96, NULL, 0, req1);
        sha1_hash("req2", 4, m->info_hash, 20, NULL, 0, req2);
        sha1_hash("req3", 4, m->secret, 96, NULL, 0, req3);
        for (int i = 0; i < 20; i++) enchash[i] = req2[i] ^ req3[i];

        uint8_t keyA[20], keyB[20];
        sha1_hash("keyA", 4, m->secret, 96, m->info_hash, 20, keyA);
        sha1_hash("keyB", 4, m->secret, 96, m->info_hash, 20, keyB);

        rc4_init(&m->rc4_out, keyA, 20);
        rc4_drop(&m->rc4_out, 1024);
        rc4_init(&m->rc4_in, keyB, 20);
        rc4_drop(&m->rc4_in, 1024);

        // Build Step 2 TX payload (BEP 8):
        // req1 (20B) || ENChash (20B) || RC4(VC (8B) || crypto_provide (4B) || padC_len (2B) || ia_len (2B) || IA (68B))
        uint8_t payload[20 + 20 + 8 + 4 + 2 + 2 + 68];
        memcpy(payload, req1, 20);
        memcpy(payload + 20, enchash, 20);

        uint8_t *enc_part = payload + 40;
        memset(enc_part, 0, 8); // VC = 8 zero bytes
        enc_part[8] = 0; enc_part[9] = 0; enc_part[10] = 0; enc_part[11] = 0x03; // crypto_provide: RC4 | Plaintext
        enc_part[12] = 0; enc_part[13] = 0; // padC_len = 0
        enc_part[14] = 0; enc_part[15] = 68; // ia_len = 68

        // BitTorrent handshake IA
        enc_part[16] = 19;
        memcpy(enc_part + 17, "BitTorrent protocol", 19);
        memset(enc_part + 36, 0, 8);
        enc_part[41] |= 0x10; // BEP 10 Extension Protocol
        enc_part[43] |= 0x05; // BEP 5 DHT + BEP 6 Fast
        memcpy(enc_part + 44, m->info_hash, 20);
        memcpy(enc_part + 64, m->peer_id, 20);

        rc4_crypt(&m->rc4_out, enc_part, 8 + 4 + 2 + 2 + 68);

        memcpy(out_tx, payload, sizeof(payload));
        *out_tx_len = sizeof(payload);

        m->phase = MSE_STATE_SENT_PAYLOAD;
        return 0;
    }

    if (m->phase == MSE_STATE_SENT_PAYLOAD) {
        // Wait for receiver response: VC (8B) + crypto_select (4B) + padD_len (2B) = 14B
        avail = *rx_len - *rx_off;
        if (avail < 14) return 0;

        uint8_t resp[14];
        memcpy(resp, rx + *rx_off, 14);
        rc4_crypt(&m->rc4_in, resp, 14);

        uint16_t padD_len = ((uint16_t)resp[12] << 8) | (uint16_t)resp[13];
        if (padD_len > 512) return -1; // invalid padding length
        if (avail < (size_t)(14 + padD_len)) return 0;

        // Skip padD if any
        if (padD_len > 0) {
            uint8_t dummy[512];
            memcpy(dummy, rx + *rx_off + 14, padD_len);
            rc4_crypt(&m->rc4_in, dummy, padD_len);
        }

        *rx_off += 14 + padD_len;
        m->phase = MSE_STATE_ESTABLISHED;

        // Decrypt remaining buffered bytes in rx (which will contain peer's BitTorrent handshake / messages)
        size_t rem = *rx_len - *rx_off;
        if (rem > 0) {
            rc4_crypt(&m->rc4_in, rx + *rx_off, rem);
        }

        return 1; // MSE Handshake Established!
    }

    return 0;
}

void mse_encrypt(mse_state *m, uint8_t *data, size_t len) {
    if (m->enabled && m->phase == MSE_STATE_ESTABLISHED) {
        rc4_crypt(&m->rc4_out, data, len);
    }
}

void mse_decrypt(mse_state *m, uint8_t *data, size_t len) {
    if (m->enabled && m->phase == MSE_STATE_ESTABLISHED) {
        rc4_crypt(&m->rc4_in, data, len);
    }
}
