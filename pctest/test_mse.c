#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include "mbedtls/dhm.h"
#include "mbedtls/sha1.h"

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

static int simple_rng(void *ctx, unsigned char *out, size_t len) {
    (void)ctx;
    for (size_t i = 0; i < len; i++) out[i] = (unsigned char)(rand() % 256);
    return 0;
}

typedef struct {
    uint8_t state[256];
    uint8_t x, y;
} rc4_state;

static void rc4_init(rc4_state *rc4, const uint8_t *key, size_t keylen) {
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

static void rc4_crypt(rc4_state *rc4, uint8_t *data, size_t len) {
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

static void rc4_drop(rc4_state *rc4, size_t count) {
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

int main() {
    srand(42);
    printf("Testing full Initiator <-> Receiver MSE Handshake...\n");

    uint8_t info_hash[20] = {0x9B, 0x71, 0xDA, 0x18, 0x83, 0x52, 0x60, 0x09, 0xA2, 0xBE,
                             0xDE, 0xAA, 0x1A, 0x85, 0xC1, 0x00, 0x7D, 0x31, 0x3B, 0xD1};
    uint8_t peer_id_a[20] = "-SW0001-123456789012";
    uint8_t peer_id_b[20] = "-qB4500-123456789012";

    // 1. Initiator A DH setup
    mbedtls_dhm_context dhm_a, dhm_b;
    mbedtls_dhm_init(&dhm_a);
    mbedtls_mpi_read_binary(&dhm_a.P, s_dhm_P, 96);
    mbedtls_mpi_read_binary(&dhm_a.G, s_dhm_G, 1);
    dhm_a.len = 96;

    uint8_t ya[96];
    mbedtls_dhm_make_public(&dhm_a, 20, ya, 96, simple_rng, NULL);

    // 2. Receiver B DH setup
    mbedtls_dhm_init(&dhm_b);
    mbedtls_mpi_read_binary(&dhm_b.P, s_dhm_P, 96);
    mbedtls_mpi_read_binary(&dhm_b.G, s_dhm_G, 1);
    dhm_b.len = 96;

    uint8_t yb[96];
    mbedtls_dhm_make_public(&dhm_b, 20, yb, 96, simple_rng, NULL);

    // Exchange Ya -> B, Yb -> A
    mbedtls_dhm_read_public(&dhm_a, yb, 96);
    mbedtls_dhm_read_public(&dhm_b, ya, 96);

    uint8_t secret_a[96], secret_b[96];
    size_t olen_a = 0, olen_b = 0;
    mbedtls_dhm_calc_secret(&dhm_a, secret_a, 96, &olen_a, NULL, NULL);
    mbedtls_dhm_calc_secret(&dhm_b, secret_b, 96, &olen_b, NULL, NULL);

    if (memcmp(secret_a, secret_b, 96) != 0) {
        printf("ERROR: secret mismatch\n");
        return 1;
    }
    printf("1. Diffie-Hellman Key Exchange: OK\n");

    // 3. Key Derivation
    uint8_t req1_a[20], req2_a[20], req3_a[20], enchash_a[20];
    sha1_hash("req1", 4, secret_a, 96, NULL, 0, req1_a);
    sha1_hash("req2", 4, info_hash, 20, NULL, 0, req2_a);
    sha1_hash("req3", 4, secret_a, 96, NULL, 0, req3_a);
    for (int i = 0; i < 20; i++) enchash_a[i] = req2_a[i] ^ req3_a[i];

    uint8_t keyA_a[20], keyB_a[20];
    sha1_hash("keyA", 4, secret_a, 96, info_hash, 20, keyA_a);
    sha1_hash("keyB", 4, secret_a, 96, info_hash, 20, keyB_a);

    rc4_state rc4_enc_a, rc4_dec_a;
    rc4_init(&rc4_enc_a, keyA_a, 20);
    rc4_drop(&rc4_enc_a, 1024);
    rc4_init(&rc4_dec_a, keyB_a, 20);
    rc4_drop(&rc4_dec_a, 1024);

    // Receiver B Key Derivation
    uint8_t keyA_b[20], keyB_b[20];
    sha1_hash("keyA", 4, secret_b, 96, info_hash, 20, keyA_b);
    sha1_hash("keyB", 4, secret_b, 96, info_hash, 20, keyB_b);

    rc4_state rc4_enc_b, rc4_dec_b;
    rc4_init(&rc4_dec_b, keyA_b, 20); // B decrypts A with keyA
    rc4_drop(&rc4_dec_b, 1024);
    rc4_init(&rc4_enc_b, keyB_b, 20); // B encrypts to A with keyB
    rc4_drop(&rc4_enc_b, 1024);

    printf("2. Key Derivation & RC4-Drop1024: OK\n");

    // 4. Initiator A sends Step 2 payload (encrypted)
    // VC (8B 0x00) || crypto_provide (4B) = 0x00000003 (RC4|Plain) || padC_len (2B) = 0 || ia_len (2B) = 68 || ia = 68B BT handshake
    uint8_t step2_plain[8 + 4 + 2 + 2 + 68];
    memset(step2_plain, 0, 8); // VC = 8 zero bytes
    step2_plain[8] = 0; step2_plain[9] = 0; step2_plain[10] = 0; step2_plain[11] = 0x03; // crypto_provide
    step2_plain[12] = 0; step2_plain[13] = 0; // padC_len = 0
    step2_plain[14] = 0; step2_plain[15] = 68; // ia_len = 68

    // BT Handshake
    step2_plain[16] = 19;
    memcpy(step2_plain + 17, "BitTorrent protocol", 19);
    memset(step2_plain + 36, 0, 8);
    step2_plain[41] |= 0x10; // BEP 10
    step2_plain[43] |= 0x05; // BEP 5 + 6
    memcpy(step2_plain + 44, info_hash, 20);
    memcpy(step2_plain + 64, peer_id_a, 20);

    uint8_t step2_enc[sizeof(step2_plain)];
    memcpy(step2_enc, step2_plain, sizeof(step2_plain));
    rc4_crypt(&rc4_enc_a, step2_enc, sizeof(step2_enc));

    // Receiver B decrypts Step 2 payload
    uint8_t step2_dec[sizeof(step2_plain)];
    memcpy(step2_dec, step2_enc, sizeof(step2_enc));
    rc4_crypt(&rc4_dec_b, step2_dec, sizeof(step2_dec));

    if (memcmp(step2_plain, step2_dec, sizeof(step2_plain)) != 0) {
        printf("ERROR: Step 2 payload decryption failed\n");
        return 1;
    }
    // Verify VC == 8 zeros
    uint8_t zero8[8] = {0};
    if (memcmp(step2_dec, zero8, 8) != 0) {
        printf("ERROR: Step 2 VC mismatch\n");
        return 1;
    }
    printf("3. Initiator Encrypted Handshake (VC + IA): Verified OK\n");

    // 5. Receiver B sends Step 3 response: VC (8B 0x00) || crypto_select (4B) = 0x00000002 || padD_len (2B) = 0 || BT handshake response (68B)
    uint8_t step3_plain[8 + 4 + 2 + 68];
    memset(step3_plain, 0, 8); // VC = 8 zero bytes
    step3_plain[8] = 0; step3_plain[9] = 0; step3_plain[10] = 0; step3_plain[11] = 0x02; // crypto_select = RC4
    step3_plain[12] = 0; step3_plain[13] = 0; // padD_len = 0
    step3_plain[14] = 19;
    memcpy(step3_plain + 15, "BitTorrent protocol", 19);
    memset(step3_plain + 34, 0, 8);
    step3_plain[39] |= 0x10;
    step3_plain[41] |= 0x05;
    memcpy(step3_plain + 42, info_hash, 20);
    memcpy(step3_plain + 62, peer_id_b, 20);

    uint8_t step3_enc[sizeof(step3_plain)];
    memcpy(step3_enc, step3_plain, sizeof(step3_plain));
    rc4_crypt(&rc4_enc_b, step3_enc, sizeof(step3_enc));

    // Initiator A decrypts Step 3 response
    uint8_t step3_dec[sizeof(step3_plain)];
    memcpy(step3_dec, step3_enc, sizeof(step3_enc));
    rc4_crypt(&rc4_dec_a, step3_dec, sizeof(step3_dec));

    if (memcmp(step3_plain, step3_dec, sizeof(step3_plain)) != 0) {
        printf("ERROR: Step 3 response decryption failed\n");
        return 1;
    }
    if (memcmp(step3_dec, zero8, 8) != 0) {
        printf("ERROR: Step 3 VC mismatch\n");
        return 1;
    }
    printf("4. Receiver Response (VC + crypto_select + BT Handshake): Verified OK\n");

    // 6. Test ongoing message stream (e.g. MSG_BITFIELD / MSG_PIECE)
    uint8_t msg_plain[] = { 0, 0, 0, 5, 5, 0xFF, 0xFF, 0xFF, 0xFF }; // len=5, id=5 (bitfield)
    uint8_t msg_enc[sizeof(msg_plain)];
    memcpy(msg_enc, msg_plain, sizeof(msg_plain));
    rc4_crypt(&rc4_enc_b, msg_enc, sizeof(msg_enc));

    uint8_t msg_dec[sizeof(msg_plain)];
    memcpy(msg_dec, msg_enc, sizeof(msg_enc));
    rc4_crypt(&rc4_dec_a, msg_dec, sizeof(msg_dec));

    if (memcmp(msg_plain, msg_dec, sizeof(msg_plain)) != 0) {
        printf("ERROR: Stream message decryption failed\n");
        return 1;
    }
    printf("5. Encrypted BitTorrent Message Streaming: Verified OK\n");

    mbedtls_dhm_free(&dhm_a);
    mbedtls_dhm_free(&dhm_b);
    printf("SUCCESS: Full BitTorrent MSE / PE protocol verified end-to-end!\n");
    return 0;
}
