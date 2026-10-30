#ifndef WEX_DHT_H
#define WEX_DHT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <netinet/in.h>

#define WEX_DHT_ID_LEN 32        /* 256-bit cryptographic identifier */
#define WEX_DHT_K 8              /* Kademlia k-bucket capacity */
#define WEX_DHT_NUM_BUCKETS 256  /* 256 buckets for 256-bit XOR space */
#define WEX_DHT_MAX_PROVIDERS 64 /* Max peers per swarm topic */
#define WEX_DHT_MAX_TOPICS 32    /* Max active spaces/topics tracked */
#define WEX_DHT_MAX_HOPS 8       /* Max relay hops for overlay transit */
#define WEX_DHT_ALPHA 3          /* Concurrency factor alpha */
#define WEX_DHT_LOOKUP_MAX 16    /* Max candidates tracked in iterative lookup */

/* DHT Node Contact */
typedef struct {
    uint8_t id[WEX_DHT_ID_LEN];
    struct sockaddr_in addr;
    uint64_t last_seen_ms;
} wex_dht_contact_t;

/* Iterative Node Lookup State Machine */
typedef struct {
    uint8_t target_id[WEX_DHT_ID_LEN];
    wex_dht_contact_t candidates[WEX_DHT_LOOKUP_MAX];
    bool queried[WEX_DHT_LOOKUP_MAX];
    size_t count;
    bool finished;
    uint64_t started_ms;
} wex_dht_lookup_t;

/* K-Bucket containing up to K contacts */
typedef struct {
    wex_dht_contact_t contacts[WEX_DHT_K];
    size_t count;
} wex_dht_bucket_t;

/* Topic provider record (who is in which space/guild) */
typedef struct {
    uint8_t peer_id[WEX_DHT_ID_LEN];
    struct sockaddr_in addr;
    uint64_t expires_at_ms;
} wex_dht_provider_t;

typedef struct {
    uint8_t topic_id[WEX_DHT_ID_LEN];
    wex_dht_provider_t providers[WEX_DHT_MAX_PROVIDERS];
    size_t provider_count;
    bool active;
} wex_dht_topic_entry_t;

/* Full Kademlia Routing Table */
typedef struct {
    uint8_t local_id[WEX_DHT_ID_LEN];
    wex_dht_bucket_t buckets[WEX_DHT_NUM_BUCKETS];
    wex_dht_topic_entry_t topics[WEX_DHT_MAX_TOPICS];
    size_t total_contacts;
} wex_dht_table_t;

/* Overlay Transit Packet for multi-hop relay */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;                    /* WEX_P2P_MAGIC */
    uint8_t  type;                     /* Packet type */
    uint8_t  src_id[WEX_DHT_ID_LEN];   /* Original sender */
    uint8_t  dst_id[WEX_DHT_ID_LEN];   /* Final destination */
    uint8_t  topic_id[WEX_DHT_ID_LEN]; /* Space / Topic ID */
    uint8_t  hops_left;                /* Decremented on each relay hop */
    uint16_t payload_len;
} wex_dht_relay_hdr_t;
#pragma pack(pop)

/* XOR distance calculation: dist = a ^ b */
void wex_dht_distance(const uint8_t a[WEX_DHT_ID_LEN],
                      const uint8_t b[WEX_DHT_ID_LEN],
                      uint8_t dist[WEX_DHT_ID_LEN]);

/* Distance comparison: returns -1 if d1 < d2, 0 if d1 == d2, +1 if d1 > d2 */
int wex_dht_dist_cmp(const uint8_t d1[WEX_DHT_ID_LEN],
                     const uint8_t d2[WEX_DHT_ID_LEN]);

/* Computes bucket index for distance: 0..255 (index of highest differing bit) */
int wex_dht_bucket_index(const uint8_t local_id[WEX_DHT_ID_LEN],
                         const uint8_t remote_id[WEX_DHT_ID_LEN]);

/* Initialize routing table with node's own 256-bit identity */
void wex_dht_init(wex_dht_table_t *table, const uint8_t local_id[WEX_DHT_ID_LEN]);

/* Update or insert contact into appropriate k-bucket */
int wex_dht_update_contact(wex_dht_table_t *table,
                           const uint8_t id[WEX_DHT_ID_LEN],
                           const struct sockaddr_in *addr,
                           uint64_t now_ms);

/* Find up to 'max_count' closest contacts to a target ID */
size_t wex_dht_find_closest(const wex_dht_table_t *table,
                            const uint8_t target_id[WEX_DHT_ID_LEN],
                            wex_dht_contact_t out_contacts[],
                            size_t max_count);

/* Announce that peer_id provides topic_id */
int wex_dht_announce_topic(wex_dht_table_t *table,
                           const uint8_t topic_id[WEX_DHT_ID_LEN],
                           const uint8_t peer_id[WEX_DHT_ID_LEN],
                           const struct sockaddr_in *addr,
                           uint64_t ttl_ms);

/* Lookup peers participating in a topic */
size_t wex_dht_get_topic_providers(wex_dht_table_t *table,
                                   const uint8_t topic_id[WEX_DHT_ID_LEN],
                                   wex_dht_provider_t out_providers[],
                                   size_t max_count);

/* Route selection for transit forwarding: selects the next hop contact closest to target */
bool wex_dht_next_hop(const wex_dht_table_t *table,
                      const uint8_t target_id[WEX_DHT_ID_LEN],
                      wex_dht_contact_t *out_next_hop);

/* Initialize an iterative lookup for target ID using closest known contacts */
void wex_dht_lookup_init(wex_dht_lookup_t *lookup,
                         const wex_dht_table_t *table,
                         const uint8_t target_id[WEX_DHT_ID_LEN],
                         uint64_t now_ms);

/* Retrieve up to 'max_queries' unqueried candidates closest to target */
size_t wex_dht_lookup_get_next(wex_dht_lookup_t *lookup,
                               size_t max_queries,
                               wex_dht_contact_t out_queries[]);

/* Incorporate results (discovered contacts) from a FIND_NODE reply into the lookup candidates */
void wex_dht_lookup_add_candidates(wex_dht_lookup_t *lookup,
                                   const wex_dht_contact_t contacts[],
                                   size_t count);

/* Check if the iterative lookup has finished (no more unqueried candidates) */
bool wex_dht_lookup_is_done(const wex_dht_lookup_t *lookup);

#endif /* WEX_DHT_H */
