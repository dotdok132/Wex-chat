#include "wex_dht.h"
#include <string.h>
#include <time.h>

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void wex_dht_distance(const uint8_t a[WEX_DHT_ID_LEN],
                      const uint8_t b[WEX_DHT_ID_LEN],
                      uint8_t dist[WEX_DHT_ID_LEN]) {
    for (int i = 0; i < WEX_DHT_ID_LEN; i++) {
        dist[i] = (uint8_t)(a[i] ^ b[i]);
    }
}

int wex_dht_dist_cmp(const uint8_t d1[WEX_DHT_ID_LEN],
                     const uint8_t d2[WEX_DHT_ID_LEN]) {
    for (int i = 0; i < WEX_DHT_ID_LEN; i++) {
        if (d1[i] < d2[i]) return -1;
        if (d1[i] > d2[i]) return 1;
    }
    return 0;
}

int wex_dht_bucket_index(const uint8_t local_id[WEX_DHT_ID_LEN],
                         const uint8_t remote_id[WEX_DHT_ID_LEN]) {
    uint8_t dist[WEX_DHT_ID_LEN];
    wex_dht_distance(local_id, remote_id, dist);

    int leading_zeros = 0;
    for (int i = 0; i < WEX_DHT_ID_LEN; i++) {
        if (dist[i] == 0) {
            leading_zeros += 8;
        } else {
            uint8_t byte = dist[i];
            int bz = 0;
            for (int b = 7; b >= 0; b--) {
                if ((byte & (1 << b)) == 0) {
                    bz++;
                } else {
                    break;
                }
            }
            leading_zeros += bz;
            break;
        }
    }

    if (leading_zeros >= 256) {
        return -1; /* Identical ID to local node */
    }

    return 255 - leading_zeros;
}

void wex_dht_init(wex_dht_table_t *table, const uint8_t local_id[WEX_DHT_ID_LEN]) {
    if (!table) return;
    memset(table, 0, sizeof(*table));
    if (local_id) {
        memcpy(table->local_id, local_id, WEX_DHT_ID_LEN);
    }
}

int wex_dht_update_contact(wex_dht_table_t *table,
                           const uint8_t id[WEX_DHT_ID_LEN],
                           const struct sockaddr_in *addr,
                           uint64_t now_ms) {
    if (!table || !id || !addr) return -1;

    int bucket_idx = wex_dht_bucket_index(table->local_id, id);
    if (bucket_idx < 0 || bucket_idx >= WEX_DHT_NUM_BUCKETS) {
        return -1; /* Self or invalid */
    }

    wex_dht_bucket_t *bucket = &table->buckets[bucket_idx];

    /* Check if contact already exists in this bucket */
    for (size_t i = 0; i < bucket->count; i++) {
        if (memcmp(bucket->contacts[i].id, id, WEX_DHT_ID_LEN) == 0) {
            bucket->contacts[i].addr = *addr;
            bucket->contacts[i].last_seen_ms = (now_ms > 0) ? now_ms : get_time_ms();
            return 0; /* Updated existing */
        }
    }

    /* If bucket has free space, add contact */
    if (bucket->count < WEX_DHT_K) {
        wex_dht_contact_t *c = &bucket->contacts[bucket->count++];
        memcpy(c->id, id, WEX_DHT_ID_LEN);
        c->addr = *addr;
        c->last_seen_ms = (now_ms > 0) ? now_ms : get_time_ms();
        table->total_contacts++;
        return 1; /* Added new contact */
    }

    /* Bucket full: replace oldest contact if stale (> 60s) */
    uint64_t oldest_time = UINT64_MAX;
    size_t oldest_idx = 0;
    for (size_t i = 0; i < bucket->count; i++) {
        if (bucket->contacts[i].last_seen_ms < oldest_time) {
            oldest_time = bucket->contacts[i].last_seen_ms;
            oldest_idx = i;
        }
    }

    uint64_t current = (now_ms > 0) ? now_ms : get_time_ms();
    if (current > oldest_time && (current - oldest_time) > 60000ULL) {
        wex_dht_contact_t *c = &bucket->contacts[oldest_idx];
        memcpy(c->id, id, WEX_DHT_ID_LEN);
        c->addr = *addr;
        c->last_seen_ms = current;
        return 2; /* Replaced stale contact */
    }

    return -2; /* Bucket full and contacts still fresh */
}

typedef struct {
    wex_dht_contact_t contact;
    uint8_t distance[WEX_DHT_ID_LEN];
} candidate_t;

size_t wex_dht_find_closest(const wex_dht_table_t *table,
                            const uint8_t target_id[WEX_DHT_ID_LEN],
                            wex_dht_contact_t out_contacts[],
                            size_t max_count) {
    if (!table || !target_id || !out_contacts || max_count == 0) return 0;

    /* Gather all active contacts from routing table */
    candidate_t pool[WEX_DHT_NUM_BUCKETS * WEX_DHT_K];
    size_t pool_size = 0;

    for (int b = 0; b < WEX_DHT_NUM_BUCKETS; b++) {
        const wex_dht_bucket_t *bucket = &table->buckets[b];
        for (size_t i = 0; i < bucket->count; i++) {
            pool[pool_size].contact = bucket->contacts[i];
            wex_dht_distance(bucket->contacts[i].id, target_id, pool[pool_size].distance);
            pool_size++;
        }
    }

    if (pool_size == 0) return 0;

    /* Partial selection sort for top 'max_count' closest */
    size_t select_count = (pool_size < max_count) ? pool_size : max_count;
    for (size_t i = 0; i < select_count; i++) {
        size_t min_idx = i;
        for (size_t j = i + 1; j < pool_size; j++) {
            if (wex_dht_dist_cmp(pool[j].distance, pool[min_idx].distance) < 0) {
                min_idx = j;
            }
        }
        if (min_idx != i) {
            candidate_t tmp = pool[i];
            pool[i] = pool[min_idx];
            pool[min_idx] = tmp;
        }
        out_contacts[i] = pool[i].contact;
    }

    return select_count;
}

int wex_dht_announce_topic(wex_dht_table_t *table,
                           const uint8_t topic_id[WEX_DHT_ID_LEN],
                           const uint8_t peer_id[WEX_DHT_ID_LEN],
                           const struct sockaddr_in *addr,
                           uint64_t ttl_ms) {
    if (!table || !topic_id || !peer_id || !addr) return -1;

    uint64_t now = get_time_ms();
    uint64_t exp = now + ((ttl_ms > 0) ? ttl_ms : 3600000ULL); /* default 1h */

    /* Find existing topic entry */
    wex_dht_topic_entry_t *target_entry = NULL;
    int free_slot = -1;

    for (int i = 0; i < WEX_DHT_MAX_TOPICS; i++) {
        if (table->topics[i].active && memcmp(table->topics[i].topic_id, topic_id, WEX_DHT_ID_LEN) == 0) {
            target_entry = &table->topics[i];
            break;
        }
        if (!table->topics[i].active && free_slot < 0) {
            free_slot = i;
        }
    }

    if (!target_entry) {
        if (free_slot < 0) return -2; /* Topics full */
        target_entry = &table->topics[free_slot];
        memcpy(target_entry->topic_id, topic_id, WEX_DHT_ID_LEN);
        target_entry->provider_count = 0;
        target_entry->active = true;
    }

    /* Check if peer already announced for this topic */
    for (size_t p = 0; p < target_entry->provider_count; p++) {
        if (memcmp(target_entry->providers[p].peer_id, peer_id, WEX_DHT_ID_LEN) == 0) {
            target_entry->providers[p].addr = *addr;
            target_entry->providers[p].expires_at_ms = exp;
            return 0;
        }
    }

    /* Add new provider */
    if (target_entry->provider_count < WEX_DHT_MAX_PROVIDERS) {
        wex_dht_provider_t *prov = &target_entry->providers[target_entry->provider_count++];
        memcpy(prov->peer_id, peer_id, WEX_DHT_ID_LEN);
        prov->addr = *addr;
        prov->expires_at_ms = exp;
        return 1;
    }

    return -3; /* Providers full for this topic */
}

size_t wex_dht_get_topic_providers(wex_dht_table_t *table,
                                   const uint8_t topic_id[WEX_DHT_ID_LEN],
                                   wex_dht_provider_t out_providers[],
                                   size_t max_count) {
    if (!table || !topic_id || !out_providers || max_count == 0) return 0;

    uint64_t now = get_time_ms();
    for (int i = 0; i < WEX_DHT_MAX_TOPICS; i++) {
        if (table->topics[i].active && memcmp(table->topics[i].topic_id, topic_id, WEX_DHT_ID_LEN) == 0) {
            size_t found = 0;
            for (size_t p = 0; p < table->topics[i].provider_count && found < max_count; p++) {
                if (now < table->topics[i].providers[p].expires_at_ms) {
                    out_providers[found++] = table->topics[i].providers[p];
                }
            }
            return found;
        }
    }

    return 0;
}

bool wex_dht_next_hop(const wex_dht_table_t *table,
                      const uint8_t target_id[WEX_DHT_ID_LEN],
                      wex_dht_contact_t *out_next_hop) {
    if (!table || !target_id || !out_next_hop) return false;

    wex_dht_contact_t closest[1];
    size_t count = wex_dht_find_closest(table, target_id, closest, 1);
    if (count == 0) return false;

    uint8_t my_dist[WEX_DHT_ID_LEN];
    uint8_t peer_dist[WEX_DHT_ID_LEN];
    wex_dht_distance(table->local_id, target_id, my_dist);
    wex_dht_distance(closest[0].id, target_id, peer_dist);

    /* Forward if closest contact is strictly closer to target than we are */
    if (wex_dht_dist_cmp(peer_dist, my_dist) < 0) {
        *out_next_hop = closest[0];
        return true;
    }

    return false; /* We are closer than any known contact */
}
