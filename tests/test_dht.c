#include "wex_dht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>

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

static void test_dht_find_node_network_query(void) {
    wex_identity_t id_a, id_b;
    assert(wex_identity_init(&id_a, NULL, "NodeA") == 0);
    assert(wex_identity_init(&id_b, NULL, "NodeB") == 0);

    wex_p2p_node_t node_a, node_b;
    assert(wex_p2p_init(&node_a, &id_a, 26001) == 0);
    assert(wex_p2p_init(&node_b, &id_b, 26002) == 0);

    /* Populate Node B's DHT routing table with 3 contacts */
    struct sockaddr_in mock_addr;
    memset(&mock_addr, 0, sizeof(mock_addr));
    mock_addr.sin_family = AF_INET;

    for (int i = 0; i < 3; i++) {
        uint8_t peer_id[32];
        memset(peer_id, 0x90 + i, 32);
        mock_addr.sin_port = htons((uint16_t)(30001 + i));
        wex_dht_update_contact(&node_b.dht, peer_id, &mock_addr, 1000);
    }
    assert(node_b.dht.total_contacts == 3);
    assert(node_a.dht.total_contacts == 0);

    /* Node A queries Node B via UDP for nodes close to 0x90 */
    struct sockaddr_in b_addr;
    memset(&b_addr, 0, sizeof(b_addr));
    b_addr.sin_family = AF_INET;
    b_addr.sin_port = htons(26002);
    inet_pton(AF_INET, "127.0.0.1", &b_addr.sin_addr);

    uint8_t search_target[32];
    memset(search_target, 0x90, 32);

    assert(wex_p2p_send_find_node(&node_a, &b_addr, search_target) == 0);

    /* Poll nodes: Node B receives FIND_NODE, runs wex_dht_find_closest, replies with NODES; Node A receives NODES */
    for (int t = 0; t < 10; t++) {
        wex_p2p_poll(&node_b, 5);
        wex_p2p_poll(&node_a, 5);
        usleep(5000);
    }

    /* Node A must now have populated its DHT routing table with the contacts returned by Node B! */
    assert(node_a.dht.total_contacts >= 3);

    TEST_PASS("Network FIND_NODE <-> NODES query-response dynamically populates DHT routing table");

    wex_p2p_close(&node_a);
    wex_p2p_close(&node_b);
}

static void test_iterative_node_lookup(void) {
    wex_identity_t id_a, id_b, id_c;
    assert(wex_identity_init(&id_a, NULL, "NodeA") == 0);
    assert(wex_identity_init(&id_b, NULL, "NodeB") == 0);
    assert(wex_identity_init(&id_c, NULL, "NodeC") == 0);

    wex_p2p_node_t node_a, node_b, node_c;
    assert(wex_p2p_init(&node_a, &id_a, 27001) == 0);
    assert(wex_p2p_init(&node_b, &id_b, 27002) == 0);
    assert(wex_p2p_init(&node_c, &id_c, 27003) == 0);

    /* Node A knows Node B */
    assert(wex_p2p_connect(&node_a, "127.0.0.1", 27002) == 0);
    /* Node B knows Node C */
    assert(wex_p2p_connect(&node_b, "127.0.0.1", 27003) == 0);

    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&node_a, 5);
        wex_p2p_poll(&node_b, 5);
        wex_p2p_poll(&node_c, 5);
        usleep(5000);
    }

    /* Node A does NOT have Node C initially */
    wex_dht_contact_t closest[1];
    size_t found = wex_dht_find_closest(&node_a.dht, id_c.sign_pk, closest, 1);
    assert(found == 1);
    assert(memcmp(closest[0].id, id_c.sign_pk, 32) != 0);

    /* Node A initiates Kademlia iterative lookup for Node C's ID */
    assert(wex_p2p_start_node_lookup(&node_a, id_c.sign_pk) == 0);

    /* Poll network for iterative convergence */
    for (int t = 0; t < 20; t++) {
        wex_p2p_poll(&node_a, 5);
        wex_p2p_poll(&node_b, 5);
        wex_p2p_poll(&node_c, 5);
        usleep(5000);
    }

    /* Node A must now have discovered Node C directly via iterative Kademlia lookup */
    found = wex_dht_find_closest(&node_a.dht, id_c.sign_pk, closest, 1);
    assert(found == 1);
    assert(memcmp(closest[0].id, id_c.sign_pk, 32) == 0);

    TEST_PASS("Kademlia Iterative Node Lookup: multi-hop discovery converges to target ID");

    wex_p2p_close(&node_a);
    wex_p2p_close(&node_b);
    wex_p2p_close(&node_c);
}

static void test_decentralized_space_discovery(void) {
    wex_identity_t id_dht, id_alice, id_bob;
    assert(wex_identity_init(&id_dht, NULL, "DHT_Node") == 0);
    assert(wex_identity_init(&id_alice, NULL, "Alice") == 0);
    assert(wex_identity_init(&id_bob, NULL, "Bob") == 0);

    wex_p2p_node_t node_dht, node_alice, node_bob;
    assert(wex_p2p_init(&node_dht, &id_dht, 28001) == 0);
    assert(wex_p2p_init(&node_alice, &id_alice, 28002) == 0);
    assert(wex_p2p_init(&node_bob, &id_bob, 28003) == 0);

    uint8_t guild_space[32];
    memset(guild_space, 0x42, 32);

    /* Alice connects to DHT node and announces her presence in guild_space */
    assert(wex_p2p_connect(&node_alice, "127.0.0.1", 28001) == 0);
    for (int t = 0; t < 10; t++) {
        wex_p2p_poll(&node_dht, 5);
        wex_p2p_poll(&node_alice, 5);
        usleep(5000);
    }
    assert(wex_p2p_announce_space(&node_alice, guild_space) >= 1);

    for (int t = 0; t < 10; t++) {
        wex_p2p_poll(&node_dht, 5);
        wex_p2p_poll(&node_alice, 5);
        usleep(5000);
    }

    /* Bob connects to DHT node and queries for providers of guild_space */
    assert(wex_p2p_connect(&node_bob, "127.0.0.1", 28001) == 0);
    for (int t = 0; t < 10; t++) {
        wex_p2p_poll(&node_dht, 5);
        wex_p2p_poll(&node_bob, 5);
        usleep(5000);
    }
    assert(wex_p2p_find_space_peers(&node_bob, guild_space) >= 1);

    /* Poll network: DHT node returns Alice to Bob; Bob connects to Alice directly */
    for (int t = 0; t < 25; t++) {
        wex_p2p_poll(&node_dht, 5);
        wex_p2p_poll(&node_alice, 5);
        wex_p2p_poll(&node_bob, 5);
        usleep(5000);
    }

    /* Bob should now be directly connected to Alice! */
    bool bob_knows_alice = false;
    for (size_t i = 0; i < node_bob.peer_count; i++) {
        if (memcmp(node_bob.peers[i].pk, id_alice.sign_pk, 32) == 0) {
            bob_knows_alice = true;
            break;
        }
    }
    assert(bob_knows_alice == true);

    TEST_PASS("Decentralized Space/Guild Provider Discovery via DHT (WEX_PKT_DHT_ANNOUNCE/PROVIDERS)");

    wex_p2p_close(&node_dht);
    wex_p2p_close(&node_alice);
    wex_p2p_close(&node_bob);
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
    test_dht_find_node_network_query();
    test_iterative_node_lookup();
    test_decentralized_space_discovery();

    printf("====================================================\n");
    printf("  ALL KADEMLIA DHT TESTS PASSED SUCCESSFULLY!       \n");
    printf("====================================================\n");
    return 0;
}
