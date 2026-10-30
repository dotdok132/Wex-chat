#ifndef WEX_MODELS_H
#define WEX_MODELS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define WEX_ID_LEN 32
#define WEX_NAME_LEN 64
#define WEX_MAX_CHANNELS 16
#define WEX_MAX_SPACES 32
#define WEX_MAX_MSG_LEN 2048

typedef struct {
    uint8_t seed[32];
    uint8_t sign_pk[32];
    uint8_t sign_sk[64];
    uint8_t x25519_pk[32];
    uint8_t x25519_sk[32];
    char nickname[WEX_NAME_LEN];
    char status[32];
} wex_identity_t;

typedef struct {
    char name[WEX_NAME_LEN];
    char topic[128];
    uint8_t type; /* 0 = text, 1 = voice */
} wex_channel_t;

typedef struct {
    uint8_t space_id[WEX_ID_LEN];
    char name[WEX_NAME_LEN];
    uint8_t owner_pk[32];
    uint8_t space_key[32]; /* E2EE symmetric channel key */
    size_t channel_count;
    wex_channel_t channels[WEX_MAX_CHANNELS];
} wex_space_t;

typedef struct {
    uint8_t space_id[WEX_ID_LEN];
    char channel[WEX_NAME_LEN];
    char author[WEX_NAME_LEN];
    uint8_t author_pk[32];
    uint64_t timestamp_ms;
    uint8_t enc_flags; /* 0 = plaintext, 1 = channel E2EE, 2 = direct DM E2EE */
    uint8_t nonce[24];
    uint8_t mac[16];
    char content[WEX_MAX_MSG_LEN];
    uint8_t signature[64];
} wex_chat_msg_t;

/* Initialize an identity from seed or generate random if seed is NULL */
int wex_identity_init(wex_identity_t *ident, const uint8_t *seed, const char *nickname);

/* Create a new space with a default #general channel */
int wex_space_create(wex_space_t *space, const char *name);

/* Add a text channel to space */
int wex_space_add_channel(wex_space_t *space, const char *channel_name);

/* Format invite code string: wex://join/<hex_id>?name=<urlencoded> */
int wex_space_get_invite(const wex_space_t *space, const char *peer_addr, char *out_buf, size_t out_size);

/* Sign a chat message using the identity's private key */
int wex_chat_msg_sign(wex_chat_msg_t *msg, const wex_identity_t *ident);

/* Verify authenticity and integrity of a chat message */
bool wex_chat_msg_verify(const wex_chat_msg_t *msg);

/* Encrypt a chat message in-place using a symmetric key (channel key or DM session key) */
int wex_chat_msg_encrypt(wex_chat_msg_t *msg, const uint8_t key[32], uint8_t enc_flag);

/* Decrypt an encrypted chat message in-place using the symmetric key */
int wex_chat_msg_decrypt(wex_chat_msg_t *msg, const uint8_t key[32]);

/* Helper to convert 32-byte binary ID to 64-character hex string */
void wex_id_to_hex(const uint8_t id[32], char hex[65]);

/* Helper to parse 64-character hex string to 32-byte binary ID */
int wex_hex_to_id(const char hex[64], uint8_t id[32]);

#endif /* WEX_MODELS_H */
