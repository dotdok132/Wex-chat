#include "wex_store.h"
#include "../../vendor/monocypher/monocypher.h"

#include <string.h>
#include <time.h>

static uint64_t current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void compute_pow_hash(const uint8_t recipient_pk[32],
                             const uint8_t *data,
                             size_t data_len,
                             uint32_t nonce,
                             uint8_t out_hash[32]) {
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 32);

    crypto_blake2b_update(&ctx, recipient_pk, 32);
    if (data && data_len > 0) {
        crypto_blake2b_update(&ctx, data, data_len);
    }
    crypto_blake2b_update(&ctx, (const uint8_t *)&nonce, sizeof(nonce));
    crypto_blake2b_final(&ctx, out_hash);
}

void wex_store_init(wex_store_t *store) {
    if (!store) return;
    memset(store, 0, sizeof(*store));
}

bool wex_pow_verify(const uint8_t recipient_pk[32], const uint8_t *data, size_t data_len, uint32_t nonce) {
    uint8_t hash[32];
    compute_pow_hash(recipient_pk, data, data_len, nonce, hash);

    /* Require 16 leading zero bits: hash[0] == 0 and hash[1] == 0 */
    return (hash[0] == 0x00 && hash[1] == 0x00);
}

uint32_t wex_pow_solve(const uint8_t recipient_pk[32], const uint8_t *data, size_t data_len) {
    uint32_t nonce = 0;
    while (!wex_pow_verify(recipient_pk, data, data_len, nonce)) {
        nonce++;
    }
    return nonce;
}

int wex_store_put(wex_store_t *store, const uint8_t recipient_pk[32],
                  const uint8_t *data, size_t data_len,
                  uint64_t ttl_ms, uint32_t pow_nonce) {
    if (!store || !recipient_pk || !data || data_len == 0 || data_len > WEX_STORE_MAX_DATA) {
        return -1;
    }

    /* Verify Proof-of-Work spam filter */
    if (!wex_pow_verify(recipient_pk, data, data_len, pow_nonce)) {
        return -2; /* PoW verification failed */
    }

    uint64_t now = current_time_ms();
    wex_store_purge_expired(store, now);

    /* Find empty slot or oldest slot */
    int target_slot = -1;
    uint64_t oldest_time = UINT64_MAX;
    int oldest_slot = 0;

    for (int i = 0; i < WEX_STORE_MAX_ITEMS; i++) {
        if (!store->items[i].occupied) {
            target_slot = i;
            break;
        }
        if (store->items[i].created_at_ms < oldest_time) {
            oldest_time = store->items[i].created_at_ms;
            oldest_slot = i;
        }
    }

    if (target_slot < 0) {
        /* Evict oldest item */
        target_slot = oldest_slot;
    } else {
        store->count++;
    }

    wex_store_item_t *item = &store->items[target_slot];
    memcpy(item->recipient_pk, recipient_pk, 32);
    item->created_at_ms = now;
    item->ttl_ms = (ttl_ms > 0) ? ttl_ms : 86400000ULL; /* default 24h */
    item->pow_nonce = pow_nonce;
    item->data_len = data_len;
    memcpy(item->data, data, data_len);
    item->occupied = true;

    return 0;
}

size_t wex_store_pull(wex_store_t *store, const uint8_t recipient_pk[32],
                      uint8_t *out_buffers[], size_t out_lens[], size_t max_items) {
    if (!store || !recipient_pk || !out_buffers || !out_lens || max_items == 0) {
        return 0;
    }

    uint64_t now = current_time_ms();
    size_t pulled = 0;

    for (int i = 0; i < WEX_STORE_MAX_ITEMS && pulled < max_items; i++) {
        if (store->items[i].occupied &&
            memcmp(store->items[i].recipient_pk, recipient_pk, 32) == 0) {

            /* Check if not expired */
            if (now <= store->items[i].created_at_ms + store->items[i].ttl_ms) {
                if (out_buffers[pulled]) {
                    memcpy(out_buffers[pulled], store->items[i].data, store->items[i].data_len);
                    out_lens[pulled] = store->items[i].data_len;
                    pulled++;
                }
            }
            store->items[i].occupied = false;
            if (store->count > 0) store->count--;
        }
    }

    return pulled;
}

void wex_store_purge_expired(wex_store_t *store, uint64_t now_ms) {
    if (!store) return;
    for (int i = 0; i < WEX_STORE_MAX_ITEMS; i++) {
        if (store->items[i].occupied) {
            if (now_ms > store->items[i].created_at_ms + store->items[i].ttl_ms) {
                store->items[i].occupied = false;
                if (store->count > 0) store->count--;
            }
        }
    }
}
