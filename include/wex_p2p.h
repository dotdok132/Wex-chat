#ifndef WEX_P2P_H
#define WEX_P2P_H

#include "wex_models.h"
#include "wex_dht.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <netinet/in.h>

#define WEX_P2P_MAGIC 0x57455831 /* "WEX1" */
#define WEX_MAX_PEERS 64
#define WEX_MAX_UDP_PAYLOAD 4096

/* Packet Types */
typedef enum {
    WEX_PKT_HELLO         = 0x01, /* Node introduces identity & nickname */
    WEX_PKT_WELCOME       = 0x02, /* Node acknowledges connection & shares info */
    WEX_PKT_PING          = 0x03, /* Keepalive / NAT hole punching */
    WEX_PKT_PONG          = 0x04, /* Keepalive response */
    WEX_PKT_JOIN_SPACE    = 0x05, /* Subscribe to space/channel swarm */
    WEX_PKT_SYNC_REQ      = 0x06, /* Request recent chat history from peers */
    WEX_PKT_SYNC_RESP     = 0x07, /* Swarm history synchronization response */
    WEX_PKT_NAT_PUNCH_REQ = 0x08, /* Request rendezvous peer to coordinate hole punching */
    WEX_PKT_NAT_PUNCH_TRIGGER = 0x09, /* Rendezvous instructs peer to punch to target */
    WEX_PKT_NAT_PUNCH     = 0x0A, /* Direct hole punch probe */
    WEX_PKT_CHAT_MSG      = 0x10, /* Direct encrypted & signed chat message */
    WEX_PKT_STORE_SHARD   = 0x20, /* Quas Store-and-Forward shard for offline peer */
    WEX_PKT_PULL_SHARDS   = 0x21, /* Offline peer querying for stored shards */
    WEX_PKT_DHT_FIND_NODE = 0x30, /* Query closest K nodes in Kademlia routing table */
    WEX_PKT_DHT_NODES     = 0x31, /* Response with closest K contacts */
    WEX_PKT_OVERLAY_RELAY = 0x32, /* Transit overlay multi-hop relay */
    WEX_PKT_DHT_ANNOUNCE  = 0x33, /* Announce topic provider to DHT */
    WEX_PKT_DHT_GET_PROVIDERS = 0x34, /* Query providers for topic from DHT */
    WEX_PKT_DHT_PROVIDERS = 0x35  /* Response with topic providers */
} wex_pkt_type_t;

/* Binary header on the wire */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;                 /* WEX_P2P_MAGIC */
    uint8_t  type;                  /* wex_pkt_type_t */
    uint8_t  sender_pk[32];         /* Ed25519 public key of sender */
    uint8_t  space_id[32];          /* Target Space ID */
    uint16_t payload_len;           /* Length of payload following header */
} wex_pkt_hdr_t;
#pragma pack(pop)

/* Peer node tracking */
typedef struct {
    struct sockaddr_in addr;
    uint8_t pk[32];
    char nickname[WEX_NAME_LEN];
    uint64_t last_seen_ms;
    bool active;
    bool synced;                    /* True if history was requested */
} wex_peer_t;

/* Callback types */
typedef void (*wex_on_msg_fn)(const wex_chat_msg_t *msg, void *user_data);
typedef void (*wex_on_peer_fn)(const wex_peer_t *peer, bool joined, void *user_data);
typedef size_t (*wex_on_sync_req_fn)(const uint8_t space_id[32], const char *channel, wex_chat_msg_t out_msgs[], size_t max_msgs, void *user_data);

/* P2P Node Engine */
typedef struct {
    int udp_fd;
    int port;
    bool running;
    wex_identity_t *identity;
    wex_peer_t peers[WEX_MAX_PEERS];
    size_t peer_count;
    uint8_t active_space_id[32];
    char active_channel[WEX_NAME_LEN];

    /* Kademlia DHT routing table */
    wex_dht_table_t dht;

    /* NAT reflexive endpoint */
    struct sockaddr_in external_addr;
    bool has_external_addr;

    /* Iterative DHT Lookups */
    wex_dht_lookup_t active_lookups[4];
    size_t lookup_count;
    uint64_t last_maintenance_ms;

    wex_on_msg_fn msg_cb;
    wex_on_peer_fn peer_cb;
    wex_on_sync_req_fn sync_cb;
    void *user_data;
} wex_p2p_node_t;

/* Initialize P2P node and DHT table */
int wex_p2p_init(wex_p2p_node_t *node, wex_identity_t *ident, int udp_port);

/* Set event callbacks */
void wex_p2p_set_callbacks(wex_p2p_node_t *node, wex_on_msg_fn msg_cb, wex_on_peer_fn peer_cb, void *user_data);

/* Set sync provider callback */
void wex_p2p_set_sync_callback(wex_p2p_node_t *node, wex_on_sync_req_fn sync_cb);

/* Connect to a remote peer (sends HELLO) - supports both IP and domain name hostnames */
int wex_p2p_connect(wex_p2p_node_t *node, const char *host_or_ip, int port);

/* Explicitly request history sync from a peer */
void wex_p2p_request_sync(wex_p2p_node_t *node, const struct sockaddr_in *target_addr);

/* Broadcast a chat message to all connected peers in the space */
int wex_p2p_broadcast_msg(wex_p2p_node_t *node, const wex_chat_msg_t *msg);

/* Transit overlay routing: forwards a message toward destination ID through next-hop in DHT */
int wex_p2p_relay_msg(wex_p2p_node_t *node, const uint8_t dst_id[32], const wex_chat_msg_t *msg);

/* Query remote peer for closest K nodes to search_id */
int wex_p2p_send_find_node(wex_p2p_node_t *node, const struct sockaddr_in *target_addr, const uint8_t search_id[32]);

/* Coordinated NAT Hole Punching: asks rendezvous_addr to broker hole punch to target_pk */
int wex_p2p_punch_hole(wex_p2p_node_t *node, const struct sockaddr_in *rendezvous_addr, const uint8_t target_pk[32]);

/* Start canonical Kademlia iterative lookup for target_id across DHT */
int wex_p2p_start_node_lookup(wex_p2p_node_t *node, const uint8_t target_id[32]);

/* Announce topic / space presence to closest DHT nodes */
int wex_p2p_announce_space(wex_p2p_node_t *node, const uint8_t space_id[32]);

/* Query closest DHT nodes to find peers in a space */
int wex_p2p_find_space_peers(wex_p2p_node_t *node, const uint8_t space_id[32]);

/* Periodic maintenance: NAT keepalive PINGs, peer timeout pruning, driving iterative lookups */
void wex_p2p_maintain(wex_p2p_node_t *node, uint64_t now_ms);

/* Process incoming UDP datagrams */
int wex_p2p_poll(wex_p2p_node_t *node, int timeout_ms);

/* Close and shutdown P2P socket */
void wex_p2p_close(wex_p2p_node_t *node);

#endif /* WEX_P2P_H */
