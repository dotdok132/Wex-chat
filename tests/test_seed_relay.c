#define _GNU_SOURCE
#include "wex_p2p.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <arpa/inet.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

typedef struct {
    int received;
    char text[WEX_MAX_MSG_LEN];
} seed_test_sink_t;

static void on_relay_test_msg(const wex_chat_msg_t *msg, void *user_data) {
    seed_test_sink_t *s = (seed_test_sink_t *)user_data;
    s->received++;
    strncpy(s->text, msg->content, sizeof(s->text) - 1);
}

static void test_seed_node_relay_and_coordination(void) {
    wex_identity_t id_seed, id_client1, id_client2;
    assert(wex_identity_init(&id_seed, NULL, "Dedicated_Seed_Node") == 0);
    assert(wex_identity_init(&id_client1, NULL, "Client_Alice") == 0);
    assert(wex_identity_init(&id_client2, NULL, "Client_Bob") == 0);

    wex_p2p_node_t seed_node, client1, client2;
    assert(wex_p2p_init(&seed_node, &id_seed, 33000) == 0);
    assert(wex_p2p_init(&client1, &id_client1, 33001) == 0);
    assert(wex_p2p_init(&client2, &id_client2, 33002) == 0);

    seed_test_sink_t sink_bob = {0};
    wex_p2p_set_callbacks(&client2, on_relay_test_msg, NULL, &sink_bob);

    /* Both clients bootstrap by connecting to the Seed Node */
    assert(wex_p2p_connect(&client1, "127.0.0.1", 33000) == 0);
    assert(wex_p2p_connect(&client2, "127.0.0.1", 33000) == 0);

    for (int t = 0; t < 20; t++) {
        wex_p2p_poll(&seed_node, 5);
        wex_p2p_poll(&client1, 5);
        wex_p2p_poll(&client2, 5);
        usleep(5000);
    }

    /* Seed node must have both clients in routing table & peer list */
    assert(seed_node.peer_count == 2);
    assert(seed_node.dht.total_contacts >= 2);

    /* 1. Alice sends an overlay transit relay message to Bob through the Seed Node */
    wex_chat_msg_t relay_msg;
    memset(&relay_msg, 0, sizeof(relay_msg));
    strncpy(relay_msg.channel, "general", 31);
    strncpy(relay_msg.content, "Packet forwarded through dedicated seed node!", WEX_MAX_MSG_LEN - 1);
    assert(wex_chat_msg_sign(&relay_msg, &id_client1) == 0);

    assert(wex_p2p_relay_msg(&client1, id_client2.sign_pk, &relay_msg) == 0);

    for (int t = 0; t < 20; t++) {
        wex_p2p_poll(&seed_node, 5);
        wex_p2p_poll(&client2, 5);
        usleep(5000);
    }

    assert(sink_bob.received == 1);
    assert(strcmp(sink_bob.text, "Packet forwarded through dedicated seed node!") == 0);

    /* 2. Alice requests the Seed Node to broker NAT hole punching to Bob */
    struct sockaddr_in seed_addr;
    memset(&seed_addr, 0, sizeof(seed_addr));
    seed_addr.sin_family = AF_INET;
    seed_addr.sin_port = htons(33000);
    inet_pton(AF_INET, "127.0.0.1", &seed_addr.sin_addr);

    assert(wex_p2p_punch_hole(&client1, &seed_addr, id_client2.sign_pk) == 0);

    for (int t = 0; t < 25; t++) {
        wex_p2p_poll(&seed_node, 5);
        wex_p2p_poll(&client1, 5);
        wex_p2p_poll(&client2, 5);
        usleep(5000);
    }

    /* Direct connection established between Alice and Bob */
    bool c1_knows_c2 = false;
    for (size_t i = 0; i < client1.peer_count; i++) {
        if (memcmp(client1.peers[i].pk, id_client2.sign_pk, 32) == 0) {
            c1_knows_c2 = true;
            break;
        }
    }
    assert(c1_knows_c2 == true);

    wex_p2p_close(&seed_node);
    wex_p2p_close(&client1);
    wex_p2p_close(&client2);

    TEST_PASS("Dedicated Seed/Relay Node routing, NAT hole punch brokering & peer bootstrapping");
}

static void test_invite_uri_and_connect_by_pk(void) {
    wex_identity_t id_seed, id_alice, id_bob;
    assert(wex_identity_init(&id_seed, NULL, "Seed_Rendezvous") == 0);
    assert(wex_identity_init(&id_alice, NULL, "Alice_Host") == 0);
    assert(wex_identity_init(&id_bob, NULL, "Bob_Joiner") == 0);

    wex_p2p_node_t seed_node, alice_node, bob_node;
    assert(wex_p2p_init(&seed_node, &id_seed, 33010) == 0);
    assert(wex_p2p_init(&alice_node, &id_alice, 33011) == 0);
    assert(wex_p2p_init(&bob_node, &id_bob, 33012) == 0);

    /* Create a guild/space on Alice's node */
    wex_space_t space;
    assert(wex_space_create(&space, "Cyberpunk_Guild") == 0);
    assert(wex_p2p_add_space(&alice_node, &space) == 0);
    assert(wex_p2p_set_active_space(&alice_node, space.space_id) == 0);

    /* Generate 1-click wex:// invite URI */
    char invite_uri[512];
    assert(wex_p2p_create_invite(&alice_node, invite_uri, sizeof(invite_uri)) == 0);
    assert(strncmp(invite_uri, "wex://join/", 11) == 0);

    /* Both Alice and Bob connect to Seed Rendezvous */
    assert(wex_p2p_connect(&alice_node, "127.0.0.1", 33010) == 0);
    assert(wex_p2p_connect(&bob_node, "127.0.0.1", 33010) == 0);

    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&seed_node, 5);
        wex_p2p_poll(&alice_node, 5);
        wex_p2p_poll(&bob_node, 5);
        usleep(3000);
    }

    /* Bob connects to Alice purely by her 64-character hex Public Key (no IP!) */
    char alice_pk_hex[65];
    wex_id_to_hex(id_alice.sign_pk, alice_pk_hex);
    assert(wex_p2p_join_invite(&bob_node, alice_pk_hex) == 0);

    for (int t = 0; t < 25; t++) {
        wex_p2p_poll(&seed_node, 5);
        wex_p2p_poll(&alice_node, 5);
        wex_p2p_poll(&bob_node, 5);
        usleep(4000);
    }

    /* Verify Bob discovered and connected directly to Alice via DHT + NAT hole punch */
    bool bob_connected_to_alice = false;
    for (size_t i = 0; i < bob_node.peer_count; i++) {
        if (memcmp(bob_node.peers[i].pk, id_alice.sign_pk, 32) == 0) {
            bob_connected_to_alice = true;
            break;
        }
    }
    assert(bob_connected_to_alice == true);

    /* Now test full wex://join/... URI importing Space ID, E2EE Space Key, and Name */
    assert(wex_p2p_join_invite(&bob_node, invite_uri) == 0);
    wex_space_t *bob_active_sp = wex_p2p_get_active_space(&bob_node);
    assert(bob_active_sp != NULL);
    assert(memcmp(bob_active_sp->space_id, space.space_id, 32) == 0);
    assert(memcmp(bob_active_sp->space_key, space.space_key, 32) == 0);

    wex_p2p_close(&seed_node);
    wex_p2p_close(&alice_node);
    wex_p2p_close(&bob_node);

    TEST_PASS("1-Click wex:// Invite URI & Zero-IP Public Key DHT Discovery + NAT Punch");
}

static void test_offline_quas_store_and_forward(void) {
    wex_identity_t id_seed, id_alice, id_bob;
    assert(wex_identity_init(&id_seed, NULL, "Quas_Seed") == 0);
    assert(wex_identity_init(&id_alice, NULL, "Alice_Sender") == 0);
    assert(wex_identity_init(&id_bob, NULL, "Bob_Offline") == 0);

    wex_p2p_node_t seed_node, alice_node, bob_node;
    assert(wex_p2p_init(&seed_node, &id_seed, 33020) == 0);
    assert(wex_p2p_init(&alice_node, &id_alice, 33021) == 0);

    /* Only Alice is online and connected to Seed Node; Bob is completely offline */
    assert(wex_p2p_connect(&alice_node, "127.0.0.1", 33020) == 0);
    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&seed_node, 5);
        wex_p2p_poll(&alice_node, 5);
        usleep(3000);
    }

    /* Alice sends an E2EE Direct Message to offline Bob */
    assert(wex_p2p_send_dm(&alice_node, id_bob.sign_pk, "Offline E2EE message via Quas Store-and-Forward!") == 0);

    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&seed_node, 5);
        wex_p2p_poll(&alice_node, 5);
        usleep(3000);
    }

    /* Seed node must have cached the PoW-verified encrypted shard for Bob */
    assert(seed_node.offline_store.count == 1);

    /* Alice goes offline! */
    wex_p2p_close(&alice_node);

    /* Later, Bob comes online and connects to Seed Node */
    assert(wex_p2p_init(&bob_node, &id_bob, 33022) == 0);
    seed_test_sink_t sink_bob = {0};
    wex_p2p_set_callbacks(&bob_node, on_relay_test_msg, NULL, &sink_bob);

    assert(wex_p2p_connect(&bob_node, "127.0.0.1", 33020) == 0);
    for (int t = 0; t < 20; t++) {
        wex_p2p_poll(&seed_node, 5);
        wex_p2p_poll(&bob_node, 5);
        usleep(3000);
    }

    /* Bob automatically receives and decrypts Alice's message even though Alice is offline! */
    assert(sink_bob.received == 1);
    assert(strcmp(sink_bob.text, "Offline E2EE message via Quas Store-and-Forward!") == 0);
    assert(seed_node.offline_store.count == 0);

    wex_p2p_close(&seed_node);
    wex_p2p_close(&bob_node);

    TEST_PASS("Quas Store-and-Forward offline E2EE message retention & automatic delivery");
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex Seed & Relay Integration Tests        \n");
    printf("====================================================\n");

    test_seed_node_relay_and_coordination();
    test_invite_uri_and_connect_by_pk();
    test_offline_quas_store_and_forward();

    printf("====================================================\n");
    printf("  ALL SEED NODE INTEGRATION TESTS PASSED!           \n");
    printf("====================================================\n");
    return 0;
}
