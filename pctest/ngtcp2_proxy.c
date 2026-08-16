#include <windows.h>

#define FWD(name) \
    __declspec(dllexport) void name(void) { \
        static FARPROC p = NULL; \
        if (!p) { \
            HMODULE h = LoadLibraryA("pctest/msys-ngtcp2_crypto_ossl-orig.dll"); \
            p = GetProcAddress(h, #name); \
        } \
        ((void(*)(void))p)(); \
    }

__declspec(dllexport) void ngtcp2_crypto_get_path_challenge_data2_cb(void) {
    static FARPROC p = NULL;
    if (!p) {
        HMODULE h = LoadLibraryA("pctest/msys-ngtcp2_crypto_ossl-orig.dll");
        p = GetProcAddress(h, "ngtcp2_crypto_get_path_challenge_data_cb");
    }
    ((void(*)(void))p)();
}

FWD(ngtcp2_crypto_aead_aes_128_gcm)
FWD(ngtcp2_crypto_aead_ctx_decrypt_init)
FWD(ngtcp2_crypto_aead_ctx_encrypt_init)
FWD(ngtcp2_crypto_aead_ctx_free)
FWD(ngtcp2_crypto_aead_init)
FWD(ngtcp2_crypto_aead_keylen)
FWD(ngtcp2_crypto_aead_noncelen)
FWD(ngtcp2_crypto_aead_retry)
FWD(ngtcp2_crypto_cipher_ctx_encrypt_init)
FWD(ngtcp2_crypto_cipher_ctx_free)
FWD(ngtcp2_crypto_client_initial_cb)
FWD(ngtcp2_crypto_ctx_initial)
FWD(ngtcp2_crypto_ctx_tls)
FWD(ngtcp2_crypto_ctx_tls_early)
FWD(ngtcp2_crypto_decrypt)
FWD(ngtcp2_crypto_decrypt_cb)
FWD(ngtcp2_crypto_delete_crypto_aead_ctx_cb)
FWD(ngtcp2_crypto_delete_crypto_cipher_ctx_cb)
FWD(ngtcp2_crypto_derive_and_install_initial_key)
FWD(ngtcp2_crypto_derive_and_install_rx_key)
FWD(ngtcp2_crypto_derive_and_install_tx_key)
FWD(ngtcp2_crypto_derive_and_install_vneg_initial_key)
FWD(ngtcp2_crypto_derive_initial_secrets)
FWD(ngtcp2_crypto_derive_packet_protection_key)
FWD(ngtcp2_crypto_encrypt)
FWD(ngtcp2_crypto_encrypt_cb)
FWD(ngtcp2_crypto_generate_regular_token)
FWD(ngtcp2_crypto_generate_regular_token2)
FWD(ngtcp2_crypto_generate_retry_token)
FWD(ngtcp2_crypto_generate_retry_token2)
FWD(ngtcp2_crypto_generate_stateless_reset_token)
FWD(ngtcp2_crypto_get_path_challenge_data_cb)
FWD(ngtcp2_crypto_hkdf)
FWD(ngtcp2_crypto_hkdf_expand)
FWD(ngtcp2_crypto_hkdf_expand_label)
FWD(ngtcp2_crypto_hkdf_extract)
FWD(ngtcp2_crypto_hp_mask)
FWD(ngtcp2_crypto_hp_mask_cb)
FWD(ngtcp2_crypto_md_hashlen)
FWD(ngtcp2_crypto_md_init)
FWD(ngtcp2_crypto_md_sha256)
FWD(ngtcp2_crypto_ossl_configure_client_session)
FWD(ngtcp2_crypto_ossl_configure_server_session)
FWD(ngtcp2_crypto_ossl_ctx_del)
FWD(ngtcp2_crypto_ossl_ctx_get_ssl)
FWD(ngtcp2_crypto_ossl_ctx_new)
FWD(ngtcp2_crypto_ossl_ctx_set_ssl)
FWD(ngtcp2_crypto_ossl_from_ngtcp2_encryption_level)
FWD(ngtcp2_crypto_ossl_from_ossl_encryption_level)
FWD(ngtcp2_crypto_ossl_init)
FWD(ngtcp2_crypto_packet_protection_ivlen)
FWD(ngtcp2_crypto_random)
FWD(ngtcp2_crypto_read_write_crypto_data)
FWD(ngtcp2_crypto_recv_client_initial_cb)
FWD(ngtcp2_crypto_recv_crypto_data_cb)
FWD(ngtcp2_crypto_recv_retry_cb)
FWD(ngtcp2_crypto_set_local_transport_params)
FWD(ngtcp2_crypto_set_remote_transport_params)
FWD(ngtcp2_crypto_update_key)
FWD(ngtcp2_crypto_update_key_cb)
FWD(ngtcp2_crypto_update_traffic_secret)
FWD(ngtcp2_crypto_verify_regular_token)
FWD(ngtcp2_crypto_verify_regular_token2)
FWD(ngtcp2_crypto_verify_retry_token)
FWD(ngtcp2_crypto_verify_retry_token2)
FWD(ngtcp2_crypto_version_negotiation_cb)
FWD(ngtcp2_crypto_write_connection_close)
FWD(ngtcp2_crypto_write_retry)
