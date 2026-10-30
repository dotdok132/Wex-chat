#ifndef WEX_BLOB_H
#define WEX_BLOB_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define WEX_BLOB_CHUNK_SIZE 1024
#define WEX_BLOB_MAX_CHUNKS 2048 /* Up to 2 MB files/avatars */
#define WEX_BLOB_HASH_LEN   32

typedef struct {
    uint8_t  root_hash[WEX_BLOB_HASH_LEN];
    char     filename[128];
    char     mime_type[64];
    uint64_t total_size;
    uint32_t total_chunks;
    uint64_t created_at_ms;
} wex_blob_info_t;

#pragma pack(push, 1)
typedef struct {
    uint8_t  root_hash[WEX_BLOB_HASH_LEN];
    uint32_t chunk_idx;
    uint32_t total_chunks;
    uint16_t data_len;
} wex_blob_chunk_hdr_t;
#pragma pack(pop)

/* Compute BLAKE2b root hash of a full memory buffer */
int wex_blob_compute_hash(const uint8_t *data, size_t size, uint8_t out_hash[WEX_BLOB_HASH_LEN]);

/* Create blob metadata from in-memory file */
int wex_blob_info_create(wex_blob_info_t *info, const char *filename,
                         const char *mime_type, const uint8_t *data, size_t size);

/* Assembly buffer for receiving chunks over P2P */
typedef struct {
    wex_blob_info_t info;
    uint8_t *data;
    uint8_t *chunk_received; /* array of booleans [total_chunks] */
    size_t received_count;
    bool completed;
} wex_blob_assembler_t;

/* Initialize an assembler for incoming blob */
int wex_blob_assembler_init(wex_blob_assembler_t *asmbler, const wex_blob_info_t *info);

/* Insert incoming chunk into assembler; returns true if blob is now fully assembled */
bool wex_blob_assembler_put_chunk(wex_blob_assembler_t *asmbler, uint32_t chunk_idx,
                                  const uint8_t *chunk_data, size_t chunk_len);

/* Free assembler buffers */
void wex_blob_assembler_free(wex_blob_assembler_t *asmbler);

/* Save assembled blob to directory: <dir>/blobs/<root_hash_hex> */
int wex_blob_save(const char *storage_dir, const wex_blob_info_t *info, const uint8_t *data);

/* Load blob from directory */
int wex_blob_load(const char *storage_dir, const uint8_t root_hash[WEX_BLOB_HASH_LEN],
                  uint8_t **out_data, size_t *out_size);

/* Base64 encoding / decoding for WebSockets & UI */
char *wex_b64_encode(const uint8_t *data, size_t input_len);
uint8_t *wex_b64_decode(const char *b64_str, size_t *out_len);

/* E2EE Blob symmetric encryption (XChaCha20-Poly1305 with authenticated 24-byte nonce & 16-byte MAC) */
int wex_blob_generate_key(uint8_t out_key[32]);
int wex_blob_encrypt_payload(const uint8_t *plaintext, size_t plain_len,
                             const uint8_t key[32],
                             uint8_t **out_ciphertext, size_t *out_cipher_len);
int wex_blob_decrypt_payload(const uint8_t *ciphertext, size_t cipher_len,
                             const uint8_t key[32],
                             uint8_t **out_plaintext, size_t *out_plain_len);

#endif /* WEX_BLOB_H */
