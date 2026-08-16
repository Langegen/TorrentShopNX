#ifndef MSE_H
#define MSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mbedtls/dhm.h"

typedef struct {
    uint8_t state[256];
    uint8_t x, y;
} mse_rc4;

typedef enum {
    MSE_STATE_IDLE = 0,
    MSE_STATE_SENT_YA,        // Sent Ya (96 bytes), waiting for Yb (96 bytes)
    MSE_STATE_SENT_PAYLOAD,   // Sent req1 + ENChash + encrypted IA, waiting for crypto_select
    MSE_STATE_ESTABLISHED,    // Stream encryption active
    MSE_STATE_PLAINTEXT       // Peer doesn't support MSE; fallback to plaintext
} mse_phase;

typedef struct {
    mse_phase phase;
    bool enabled;
    bool handshaked;          // BT handshake received inside MSE or plaintext
    mbedtls_dhm_context dhm;
    mse_rc4 rc4_in;
    mse_rc4 rc4_out;
    uint8_t info_hash[20];
    uint8_t peer_id[20];
    uint8_t ya[96];
    uint8_t secret[96];
} mse_state;

void mse_init(mse_state *m, const uint8_t info_hash[20], const uint8_t peer_id[20]);
void mse_free(mse_state *m);

// Step 1: Generate Ya (96 bytes) into out_buf (returns bytes written, e.g. 96)
int mse_create_ya(mse_state *m, uint8_t *out_buf, size_t max_len);

// Step 2 & 3: Process incoming handshake bytes from socket.
// If peer sends Yb, computes S, derives keys, generates Step 2 payload into out_tx.
// Once synced with crypto_select, returns 1, sets *handshaked = true.
// If peer sends standard plaintext handshake "\x13BitTorrent protocol", sets *is_plaintext_fallback = true and phase = MSE_STATE_PLAINTEXT.
int mse_process_rx(mse_state *m,
                   uint8_t *rx, size_t *rx_len, size_t *rx_off,
                   uint8_t *out_tx, size_t *out_tx_len,
                   bool *is_plaintext_fallback);

void mse_encrypt(mse_state *m, uint8_t *data, size_t len);
void mse_decrypt(mse_state *m, uint8_t *data, size_t len);

#endif // MSE_H
