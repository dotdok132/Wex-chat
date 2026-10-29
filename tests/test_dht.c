#include "wex_dht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

static void test_xor_metric(void) {
    uint8_t a[32], b[32], c[32];
    memset(a, 0x11, 32);
    memset(b, 0x22, 32);
    memset(c, 0x33, 32);

    /* 1. Identity: d(a, a) == 0 */
    uint8_t dist_aa[32];
    wex_dht_distance(a, a, dist_aa);
    for (int i = 0; i < 32; i++) {
        assert(dist_aa[i] == 0);
    }

    /* 2. Symmetry: d(a, b) == d(b, a) */
    uint8_t dist_ab[32], dist_ba[32];
    wex_dht_distance(a, b, dist_ab);
    wex_dht_distance(b, a, dist_ba);
    assert(memcmp(dist_ab, dist_ba, 32) == 0);
    assert(wex_dht_dist_cmp(dist_ab, dist_ba) == 0);

    /* 3. Distance comparison */
    uint8_t near_id[32], far_id[32];
    memset(near_id, 0x10, 32); /* dist to a is 0x01 */
    memset(far_id, 0xF0, 32);  /* dist to a is 0xE1 */

    uint8_t d_near[32], d_far[32];
    wex_dht_distance(a, near_id, d_near);
    wex_dht_distance(a, far_id, d_far);
    assert(wex_dht_dist_cmp(d_near, d_far) < 0);
    assert(wex_dht_dist_cmp(d_far, d_near) > 0);

    TEST_PASS("XOR Metric Properties (identity, symmetry, strict ordering)");
}

static void test_bucket_indexing(void) {
    uint8_t local[32];
    memset(local, 0, 32);

    /* Identical ID must return -1 */
    assert(wex_dht_bucket_index(local, local) == -1);

    /* Highest bit of MSB differs: 1000 0000 -> bucket 255 */
    uint8_t remote_high[32];
    memset(remote_high, 0, 32);
    remote_high[0] = 0x80;
    assert(wex_dht_bucket_index(local, remote_high) == 255);

    /* Second highest bit differs: 0100 0000 -> bucket 254 */
    uint8_t remote_254[32];
    memset(remote_254, 0, 32);
    remote_254[0] = 0x40;
    assert(wex_dht_bucket_index(local, remote_254) == 254);

    /* Lowest bit of LSB differs: byte 31 = 0x01 -> bucket 0 */
    uint8_t remote_low[32];
    memset(remote_low, 0, 32);
    remote_low[31] = 0x01;
    assert(wex_dht_bucket_index(local, remote_low) == 0);

    TEST_PASS("Kademlia 256-bucket index determination");
}

static void test_routing_table_and_closest_search(void) {
    wex_dht_table_t table;
    uint8_t my_id[32];
    memset(my_id, 0x55, 32);
    wex_dht_init(&table, my_id);

    /* Insert 15 contacts across diverse distances */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9001);

    for (int i = 0; i < 15; i++) {
        uint8_t peer_id[32];
        memset(peer_id, 0x55, 32);
        peer_id[i] ^= 0x80; /* Flip highest bit of byte i to land in distinct buckets */

        addr.sin_port = htons((uint16_t)(9000 + i));
        int res = wex_dht_update_contact(&table, peer_id, &addr, 1000);
        assert(res == 1); /* successfully inserted */
    }

    assert(table.total_contacts == 15);

    /* Updating existing contact */
    uint8_t existing_peer[32];
    memset(existing_peer, 0x55, 32);
    existing_peer[0] ^= 0x80;
    addr.sin_port = htons(9999);
    assert(wex_dht_update_contact(&table, existing_peer, &addr, 2000) == 0);

    /* Find closest 4 contacts to target (target is close to peer 0) */
    uint8_t target[32];
    memset(target, 0x55, 32);
    target[0] ^= 0x80; /* Exact match with peer 0 */

    wex_dht_contact_t closest[4];
    size_t found = wex_dht_find_closest(&table, target, closest, 4);
    assert(found == 4);

    /* First closest must be exact match (dist = 0) */
    assert(memcmp(closest[0].id, target, 32) == 0);

    /* Verify results are sorted by ascending XOR distance */
    for (size_t i = 0; i < found - 1; i++) {
        uint8_t d1[32], d2[32];
        wex_dht_distance(closest[i].id, target, d1);
        wex_dht_distance(closest[i + 1].id, target, d2);
        assert(wex_dht_dist_cmp(d1, d2) <= 0);
    }

    TEST_PASS("Kademlia k-bucket insertion, LRU update, and K-closest node search");
}

static void test_swarm_topic_providers(void) {
    wex_dht_table_t table;
    wex_dht_init(&table, NULL);

    uint8_t topic_id[32];
    memset(topic_id, 0xAA, 32); /* Space ID */

    uint8_t peer1_id[32], peer2_id[32];
    memset(peer1_id, 0x01, 32);
    memset(peer2_id, 0x02, 32);

    struct sockaddr_in addr1, addr2;
    memset(&addr1, 0, sizeof(addr1));
    memset(&addr2, 0, sizeof(addr2));
    addr1.sin_family = AF_INET;
    addr1.sin_port = htons(11001);
    addr2.sin_family = AF_INET;
    addr2.sin_port = htons(11002);

    assert(wex_dht_announce_topic(&table, topic_id, peer1_id, &addr1, 60000) == 1);
    assert(wex_dht_announce_topic(&table, topic_id, peer2_id, &addr2, 60000) == 1);

    wex_dht_provider_t providers[8];
    size_t count = wex_dht_get_topic_providers(&table, topic_id, providers, 8);
    assert(count == 2);
    assert(memcmp(providers[0].peer_id, peer1_id, 32) == 0);
    assert(memcmp(providers[1].peer_id, peer2_id, 32) == 0);

    TEST_PASS("Swarm Topic Announcement & Provider Discovery (Decentralized Guild Members)");
}

static void test_transit_relay_next_hop(void) {
    wex_dht_table_t table;
    uint8_t my_id[32];
    memset(my_id, 0x00, 32);
    wex_dht_init(&table, my_id);

    /* Our routing table knows intermediate Node B */
    uint8_t node_b[32];
    memset(node_b, 0x00, 32);
    node_b[0] = 0x70; /* Closer to target 0x7F than my_id 0x00 */

    struct sockaddr_in b_addr;
    memset(&b_addr, 0, sizeof(b_addr));
    b_addr.sin_port = htons(12345);
    wex_dht_update_contact(&table, node_b, &b_addr, 1000);

    /* Target is Node C at 0x7F */
    uint8_t target_c[32];
    memset(target_c, 0x00, 32);
    target_c[0] = 0x7F;

    wex_dht_contact_t next_hop;
    bool should_forward = wex_dht_next_hop(&table, target_c, &next_hop);
    assert(should_forward == true);
    assert(memcmp(next_hop.id, node_b, 32) == 0);
    assert(next_hop.addr.sin_port == htons(12345));

    TEST_PASS("Overlay Transit Relay: Next-Hop selection moves packets closer to destination");
}

#include "wex_p2p.h"
#include <unistd.h>

typedef struct {
    int received_count;
    char last_content[WEX_MAX_MSG_LEN];
} relay_test_sink_t;

static void on_relay_msg(const wex_chat_msg_t *msg, void *user_data) {
    relay_test_sink_t *s = (relay_test_sink_t *)user_data;
    s->received_count++;
    strncpy(s->last_content, msg->content, sizeof(s->last_content) - 1);
}

static void test_end_to_end_transit_relay(void) {
    wex_identity_t id_a, id_b, id_c;
    assert(wex_identity_init(&id_a, NULL, "NodeA") == 0);
    assert(wex_identity_init(&id_b, NULL, "NodeB_Relay") == 0);
    assert(wex_identity_init(&id_c, NULL, "NodeC_Dest") == 0);

    wex_p2p_node_t node_a, node_b, node_c;
    assert(wex_p2p_init(&node_a, &id_a, 25001) == 0);
    assert(wex_p2p_init(&node_b, &id_b, 25002) == 0);
    assert(wex_p2p_init(&node_c, &id_c, 25003) == 0);

    relay_test_sink_t sink_c = {0};
    wex_p2p_set_callbacks(&node_c, on_relay_msg, NULL, &sink_c);

    /* Node A connects to Node B */
    assert(wex_p2p_connect(&node_a, "127.0.0.1", 25002) == 0);
    /* Node C connects to Node B */
    assert(wex_p2p_connect(&node_c, "127.0.0.1", 25002) == 0);

    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&node_a, 5);
        wex_p2p_poll(&node_b, 5);
        wex_p2p_poll(&node_c, 5);
        usleep(5000);
    }

    /* Node A knows Node B. Node B knows Node C. Node A does NOT have Node C in its peer list! */
    assert(node_a.peer_count == 1);
    assert(node_b.peer_count == 2);
    assert(node_c.peer_count == 1);

    /* Node A sends message routed to Node C via overlay relay */
    wex_chat_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, "general", 31);
    strncpy(msg.content, "Multi-hop overlay message reached destination without direct IP!", WEX_MAX_MSG_LEN - 1);
    assert(wex_chat_msg_sign(&msg, &id_a) == 0);

    assert(wex_p2p_relay_msg(&node_a, id_c.sign_pk, &msg) == 0);

    /* Poll nodes: Node B forwards packet to Node C */
    for (int t = 0; t < 20; t++) {
        wex_p2p_poll(&node_b, 5);
        wex_p2p_poll(&node_c, 5);
        usleep(5000);
    }

    assert(sink_c.received_count == 1);
    assert(strcmp(sink_c.last_content, "Multi-hop overlay message reached destination without direct IP!") == 0);

    TEST_PASS("End-to-End Multi-Hop Overlay Transit Relay (Node A -> Node B -> Node C)");

    wex_p2p_close(&node_a);
    wex_p2p_close(&node_b);
    wex_p2p_close(&node_c);
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex Kademlia DHT & Overlay Routing Tests  \n");
    printf("====================================================\n");

    test_xor_metric();
    test_bucket_indexing();
    test_routing_table_and_closest_search();
    test_swarm_topic_providers();
    test_transit_relay_next_hop();
    test_end_to_end_transit_relay();

    printf("====================================================\n");
    printf("  ALL KADEMLIA DHT TESTS PASSED SUCCESSFULLY!       \n");
    printf("====================================================\n");
    return 0;
}
