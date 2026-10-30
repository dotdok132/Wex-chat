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

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex Seed & Relay Integration Tests        \n");
    printf("====================================================\n");

    test_seed_node_relay_and_coordination();

    printf("====================================================\n");
    printf("  ALL SEED NODE INTEGRATION TESTS PASSED!           \n");
    printf("====================================================\n");
    return 0;
}
