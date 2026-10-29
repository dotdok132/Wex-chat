#ifndef WEX_STORE_H
#define WEX_STORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define WEX_STORE_MAX_ITEMS 256
#define WEX_STORE_MAX_DATA  1024
#define WEX_POW_DIFFICULTY_BITS 16 /* 2 leading zero bytes */

typedef struct {
    uint8_t recipient_pk[32];
    uint64_t created_at_ms;
    uint64_t ttl_ms;
    uint32_t pow_nonce;
    size_t data_len;
    uint8_t data[WEX_STORE_MAX_DATA];
    bool occupied;
} wex_store_item_t;

typedef struct {
    wex_store_item_t items[WEX_STORE_MAX_ITEMS];
    size_t count;
} wex_store_t;

/* Initialize store */
void wex_store_init(wex_store_t *store);

/* Compute Proof-of-Work nonce for a payload to be cached */
uint32_t wex_pow_solve(const uint8_t recipient_pk[32], const uint8_t *data, size_t data_len);

/* Verify Proof-of-Work difficulty */
bool wex_pow_verify(const uint8_t recipient_pk[32], const uint8_t *data, size_t data_len, uint32_t nonce);

/* Store an item for an offline recipient (checks PoW & TTL) */
int wex_store_put(wex_store_t *store, const uint8_t recipient_pk[32],
                  const uint8_t *data, size_t data_len,
                  uint64_t ttl_ms, uint32_t pow_nonce);

/* Pull and remove all stored items for a specific recipient */
size_t wex_store_pull(wex_store_t *store, const uint8_t recipient_pk[32],
                      uint8_t *out_buffers[], size_t out_lens[], size_t max_items);

/* Clean expired items */
void wex_store_purge_expired(wex_store_t *store, uint64_t now_ms);

#endif /* WEX_STORE_H */
