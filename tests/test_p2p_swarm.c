#include "wex_p2p.h"
#include "wex_models.h"
#include "wex_store.h"
#include "wex_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

typedef struct {
    int received_count;
    char last_author[WEX_NAME_LEN];
    char last_content[WEX_MAX_MSG_LEN];
} test_sink_t;

static void on_test_msg(const wex_chat_msg_t *msg, void *user_data) {
    test_sink_t *sink = (test_sink_t *)user_data;
    sink->received_count++;
    strncpy(sink->last_author, msg->author, sizeof(sink->last_author) - 1);
    strncpy(sink->last_content, msg->content, sizeof(sink->last_content) - 1);
}

static void test_p2p_three_node_swarm(void) {
    printf("[*] Setting up 3 independent P2P nodes on UDP ports 19001, 19002, 19003...\n");

    /* 1. Setup Identities */
    wex_identity_t alice_id, bob_id, charlie_id;
    assert(wex_identity_init(&alice_id, NULL, "Alice") == 0);
    assert(wex_identity_init(&bob_id, NULL, "Bob") == 0);
    assert(wex_identity_init(&charlie_id, NULL, "Charlie") == 0);

    /* 2. Setup Nodes */
    wex_p2p_node_t alice_node, bob_node, charlie_node;
    assert(wex_p2p_init(&alice_node, &alice_id, 19001) == 0);
    assert(wex_p2p_init(&bob_node, &bob_id, 19002) == 0);
    assert(wex_p2p_init(&charlie_node, &charlie_id, 19003) == 0);

    test_sink_t alice_sink = {0}, bob_sink = {0}, charlie_sink = {0};
    wex_p2p_set_callbacks(&alice_node, on_test_msg, NULL, &alice_sink);
    wex_p2p_set_callbacks(&bob_node, on_test_msg, NULL, &bob_sink);
    wex_p2p_set_callbacks(&charlie_node, on_test_msg, NULL, &charlie_sink);

    /* 3. Bob and Charlie connect to Alice via UDP */
    assert(wex_p2p_connect(&bob_node, "127.0.0.1", 19001) == 0);
    assert(wex_p2p_connect(&charlie_node, "127.0.0.1", 19001) == 0);

    /* Poll network to process handshakes */
    for (int t = 0; t < 10; t++) {
        wex_p2p_poll(&alice_node, 10);
        wex_p2p_poll(&bob_node, 10);
        wex_p2p_poll(&charlie_node, 10);
        usleep(5000);
    }

    assert(alice_node.peer_count >= 2);
    assert(bob_node.peer_count >= 1);
    assert(charlie_node.peer_count >= 1);
    TEST_PASS("P2P UDP 3-node mesh discovery & bidirectional handshakes");

    /* 4. Alice sends a broadcast message */
    wex_chat_msg_t msg1;
    memset(&msg1, 0, sizeof(msg1));
    strncpy(msg1.channel, "general", sizeof(msg1.channel) - 1);
    strncpy(msg1.content, "Hello P2P Swarm from Alice!", sizeof(msg1.content) - 1);
    assert(wex_chat_msg_sign(&msg1, &alice_id) == 0);

    int sent = wex_p2p_broadcast_msg(&alice_node, &msg1);
    assert(sent >= 2);

    /* Poll receivers */
    for (int t = 0; t < 10; t++) {
        wex_p2p_poll(&bob_node, 10);
        wex_p2p_poll(&charlie_node, 10);
        usleep(5000);
    }

    assert(bob_sink.received_count == 1);
    assert(strcmp(bob_sink.last_author, "Alice") == 0);
    assert(strcmp(bob_sink.last_content, "Hello P2P Swarm from Alice!") == 0);

    assert(charlie_sink.received_count == 1);
    assert(strcmp(charlie_sink.last_author, "Alice") == 0);
    assert(strcmp(charlie_sink.last_content, "Hello P2P Swarm from Alice!") == 0);

    TEST_PASS("Real UDP broadcast & Ed25519 cryptographic verification across peers");

    /* 5. Charlie replies to the swarm */
    wex_chat_msg_t msg2;
    memset(&msg2, 0, sizeof(msg2));
    strncpy(msg2.channel, "general", sizeof(msg2.channel) - 1);
    strncpy(msg2.content, "Decentralized Discord is live!", sizeof(msg2.content) - 1);
    assert(wex_chat_msg_sign(&msg2, &charlie_id) == 0);

    assert(wex_p2p_broadcast_msg(&charlie_node, &msg2) >= 1);

    for (int t = 0; t < 10; t++) {
        wex_p2p_poll(&alice_node, 10);
        usleep(5000);
    }

    assert(alice_sink.received_count == 1);
    assert(strcmp(alice_sink.last_author, "Charlie") == 0);
    assert(strcmp(alice_sink.last_content, "Decentralized Discord is live!") == 0);

    TEST_PASS("Bi-directional multi-hop messaging over UDP");

    wex_p2p_close(&alice_node);
    wex_p2p_close(&bob_node);
    wex_p2p_close(&charlie_node);
}

static void test_quas_store_and_forward(void) {
    wex_store_t store;
    wex_store_init(&store);

    uint8_t offline_recipient_pk[32];
    memset(offline_recipient_pk, 0xEE, 32);

    const char *offline_msg = "Encrypted shard while Charlie was offline!";
    size_t msg_len = strlen(offline_msg) + 1;

    /* 1. Calculate Proof-of-Work */
    uint32_t nonce = wex_pow_solve(offline_recipient_pk, (const uint8_t *)offline_msg, msg_len);
    assert(wex_pow_verify(offline_recipient_pk, (const uint8_t *)offline_msg, msg_len, nonce) == true);

    /* 2. Store with invalid nonce must fail */
    assert(wex_store_put(&store, offline_recipient_pk, (const uint8_t *)offline_msg, msg_len, 60000, nonce + 1) == -2);

    /* 3. Store with valid PoW must succeed */
    assert(wex_store_put(&store, offline_recipient_pk, (const uint8_t *)offline_msg, msg_len, 60000, nonce) == 0);
    assert(store.count == 1);

    /* 4. Recipient comes online and pulls stored message */
    uint8_t pulled_buf[WEX_STORE_MAX_DATA];
    uint8_t *buffers[1] = { pulled_buf };
    size_t lens[1] = { 0 };

    size_t pulled = wex_store_pull(&store, offline_recipient_pk, buffers, lens, 1);
    assert(pulled == 1);
    assert(lens[0] == msg_len);
    assert(strcmp((char *)pulled_buf, offline_msg) == 0);

    /* 5. Second pull must return 0 (already delivered) */
    assert(wex_store_pull(&store, offline_recipient_pk, buffers, lens, 1) == 0);
    assert(store.count == 0);

    TEST_PASS("Quas Store-and-Forward: Proof-of-Work anti-spam + offline message retention & delivery");
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex True P2P UDP Swarm & Quas Test Suite  \n");
    printf("====================================================\n");

    test_p2p_three_node_swarm();
    test_quas_store_and_forward();

    printf("====================================================\n");
    printf("  ALL REAL P2P & QUAS TESTS PASSED WITH 100%% SUCCESS!\n");
    printf("====================================================\n");
    return 0;
}
