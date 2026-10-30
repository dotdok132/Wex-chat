#define _GNU_SOURCE
#include "wex_p2p.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

typedef struct {
    int count;
    char last_ch[WEX_NAME_LEN];
    char last_text[WEX_MAX_MSG_LEN];
} channel_sink_t;

static void on_test_msg(const wex_chat_msg_t *msg, void *user_data) {
    channel_sink_t *s = (channel_sink_t *)user_data;
    s->count++;
    strncpy(s->last_ch, msg->channel, sizeof(s->last_ch) - 1);
    strncpy(s->last_text, msg->content, sizeof(s->last_text) - 1);
}

static void test_multi_channel_sync(void) {
    wex_identity_t id_alice, id_bob;
    assert(wex_identity_init(&id_alice, NULL, "Alice_Admin") == 0);
    assert(wex_identity_init(&id_bob, NULL, "Bob_Member") == 0);

    wex_p2p_node_t node_a, node_b;
    assert(wex_p2p_init(&node_a, &id_alice, 32001) == 0);
    assert(wex_p2p_init(&node_b, &id_bob, 32002) == 0);

    channel_sink_t sink_b = {0};
    wex_p2p_set_callbacks(&node_b, on_test_msg, NULL, &sink_b);

    /* Alice creates Space with multiple channels */
    wex_space_t space;
    assert(wex_space_create(&space, "Devs Guild") == 0);
    assert(wex_space_add_channel(&space, "offtopic") == 0);
    assert(space.channel_count == 3); /* general, dev, offtopic */

    assert(wex_p2p_add_space(&node_a, &space) == 0);
    assert(wex_p2p_set_active_space(&node_a, space.space_id) == 0);

    /* Bob connects to Alice */
    assert(wex_p2p_connect(&node_b, "127.0.0.1", 32001) == 0);

    for (int t = 0; t < 20; t++) {
        wex_p2p_poll(&node_a, 5);
        wex_p2p_poll(&node_b, 5);
        usleep(5000);
    }

    /* Bob must have received Space Info with all 3 channels */
    assert(node_b.space_count >= 1);
    wex_space_t *b_sp = &node_b.spaces[0];
    assert(strcmp(b_sp->name, "Devs Guild") == 0);
    assert(b_sp->channel_count == 3);
    assert(strcmp(b_sp->channels[0].name, "general") == 0);
    assert(strcmp(b_sp->channels[1].name, "dev") == 0);
    assert(strcmp(b_sp->channels[2].name, "offtopic") == 0);

    /* Alice sends message to #dev */
    assert(wex_p2p_set_active_channel(&node_a, "dev") == 0);

    wex_chat_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    memcpy(msg.space_id, space.space_id, 32);
    strncpy(msg.channel, "dev", 31);
    strncpy(msg.content, "Reviewing pull request in #dev", WEX_MAX_MSG_LEN - 1);
    assert(wex_chat_msg_sign(&msg, &id_alice) == 0);

    assert(wex_p2p_broadcast_msg(&node_a, &msg) >= 1);

    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&node_b, 5);
        usleep(5000);
    }

    assert(sink_b.count == 1);
    assert(strcmp(sink_b.last_ch, "dev") == 0);
    assert(strcmp(sink_b.last_text, "Reviewing pull request in #dev") == 0);

    /* Alice dynamically creates channel #announcements */
    assert(wex_p2p_create_channel(&node_a, "announcements") == 0);

    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&node_b, 5);
        usleep(5000);
    }

    /* Bob must now have 4 channels */
    assert(b_sp->channel_count == 4);
    assert(strcmp(b_sp->channels[3].name, "announcements") == 0);

    TEST_PASS("Spaces & Multi-Channel Dynamic Sync and Channel-scoped Messaging");

    wex_p2p_close(&node_a);
    wex_p2p_close(&node_b);
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex Spaces & Channels Tests               \n");
    printf("====================================================\n");

    test_multi_channel_sync();

    printf("====================================================\n");
    printf("  ALL SPACES & CHANNELS TESTS PASSED SUCCESSFULLY!  \n");
    printf("====================================================\n");
    return 0;
}
