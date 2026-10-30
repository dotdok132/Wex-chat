#ifndef WEX_CRYPTO_H
#define WEX_CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define WEX_KEY_LEN 32
#define WEX_NONCE_LEN 24
#define WEX_MAC_LEN 16
#define WEX_CHECKSUM_LEN 16

/* Authenticated share of a 32-byte secret */
typedef struct {
    uint8_t index;                  /* 1-based x coordinate (1..255) */
    uint8_t threshold;              /* k: required shares to reconstruct */
    uint8_t total;                  /* n: total shares generated */
    uint8_t data[WEX_KEY_LEN];      /* Evaluated y values */
    uint8_t checksum[WEX_CHECKSUM_LEN]; /* BLAKE2b verification tag */
} wex_share_t;

/* Cryptographically secure random bytes */
int wex_randombytes(void *buf, size_t len);

/* Memory wiper for sensitive material */
void wex_clean(void *buf, size_t len);

/* Initialize GF(2^8) math tables */
void wex_gf256_init(void);

/* Shamir Secret Sharing API */
int wex_shamir_split(const uint8_t secret[WEX_KEY_LEN],
                     uint8_t k,
                     uint8_t n,
                     wex_share_t *shares_out);

bool wex_shamir_verify_share(const wex_share_t *share);

int wex_shamir_combine(const wex_share_t *shares[],
                       uint8_t num_shares,
                       uint8_t recovered_secret[WEX_KEY_LEN]);

/* Symmetric authenticated encryption (ChaCha20-Poly1305 with 24-byte nonce) */
int wex_encrypt(const uint8_t key[WEX_KEY_LEN],
                const uint8_t *plaintext,
                size_t len,
                uint8_t *ciphertext,
                uint8_t nonce[WEX_NONCE_LEN],
                uint8_t mac[WEX_MAC_LEN]);

int wex_decrypt(const uint8_t key[WEX_KEY_LEN],
                const uint8_t *ciphertext,
                size_t len,
                const uint8_t nonce[WEX_NONCE_LEN],
                const uint8_t mac[WEX_MAC_LEN],
                uint8_t *plaintext);

/* X25519 Diffie-Hellman Key Exchange: shared_secret = X25519(my_sk, peer_pk) */
int wex_ecdh(const uint8_t my_x25519_sk[WEX_KEY_LEN],
             const uint8_t peer_x25519_pk[WEX_KEY_LEN],
             uint8_t shared_secret[WEX_KEY_LEN]);

/* Session Key Derivation (BLAKE2b KDF) from shared secret and context info */
int wex_kdf_session_key(const uint8_t shared_secret[WEX_KEY_LEN],
                        const uint8_t salt[16],
                        const char *context,
                        uint8_t out_key[WEX_KEY_LEN]);

#endif /* WEX_CRYPTO_H */
