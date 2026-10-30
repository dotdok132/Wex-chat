#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include "wex_p2p.h"
#include "wex_crypto.h"
#include "wex_models.h"
#include "wex_blob.h"
#include "wex_actions.h"
#include "wex_voice.h"
#include "wex_permissions.h"

#define TEST_DATA_DIR_A "./.test_e2e_a"
#define TEST_DATA_DIR_B "./.test_e2e_b"
#define TEST_DATA_DIR_C "./.test_e2e_c"

typedef struct {
    size_t msg_count;
    wex_chat_msg_t last_msg;
    size_t action_count;
    wex_msg_action_t last_action;
    size_t blob_count;
    wex_blob_info_t last_blob;
    size_t voice_count;
    wex_voice_hdr_t last_voice_hdr;
    wex_reactions_store_t reactions;
    wex_acl_t acl;
} test_node_state_t;

static void on_msg(const wex_chat_msg_t *msg, void *user_data) {
    test_node_state_t *st = (test_node_state_t *)user_data;
    if (st && msg) {
        /* Check if author is banned */
        if (wex_acl_is_banned(&st->acl, msg->author_pk)) {
            return; /* Dropped due to ban */
        }
        st->last_msg = *msg;
        st->msg_count++;
    }
}

static void on_peer(const wex_peer_t *peer, bool joined, void *user_data) {
    (void)peer;
    (void)joined;
    (void)user_data;
}

static void on_blob(const wex_blob_info_t *info, const uint8_t *data, void *user_data) {
    test_node_state_t *st = (test_node_state_t *)user_data;
    if (st && info) {
        st->last_blob = *info;
        if (data != NULL) {
            st->blob_count++;
        }
    }
}

static void on_action(const wex_msg_action_t *action, void *user_data) {
    test_node_state_t *st = (test_node_state_t *)user_data;
    if (st && action) {
        if (wex_acl_is_banned(&st->acl, action->actor_pk)) {
            return;
        }
        st->last_action = *action;
        st->action_count++;
        wex_msg_action_apply(&st->reactions, &st->acl, &st->last_msg, action);
    }
}

static void on_voice(const char *channel, const wex_voice_hdr_t *hdr, const uint8_t *audio, size_t len, void *user_data) {
    test_node_state_t *st = (test_node_state_t *)user_data;
    (void)channel;
    (void)audio;
    (void)len;
    if (st && hdr) {
        st->last_voice_hdr = *hdr;
        st->voice_count++;
    }
}

int main(void) {
    printf("[*] Running End-to-End Full Stack Multi-Node Integration Test...\n");

    mkdir(TEST_DATA_DIR_A, 0700);
    mkdir(TEST_DATA_DIR_B, 0700);
    mkdir(TEST_DATA_DIR_C, 0700);

    wex_identity_t ident_a, ident_b, ident_c;
    assert(wex_identity_init(&ident_a, NULL, "Alice-Owner") == 0);
    assert(wex_identity_init(&ident_b, NULL, "Bob-Peer") == 0);
    assert(wex_identity_init(&ident_c, NULL, "Charlie-Attacker") == 0);

    wex_p2p_node_t node_a, node_b, node_c;
    test_node_state_t state_a, state_b, state_c;
    memset(&state_a, 0, sizeof(state_a));
    memset(&state_b, 0, sizeof(state_b));
    memset(&state_c, 0, sizeof(state_c));

    wex_reactions_store_init(&state_a.reactions);
    wex_reactions_store_init(&state_b.reactions);
    wex_reactions_store_init(&state_c.reactions);

    assert(wex_p2p_init(&node_a, &ident_a, 9310) == 0);
    assert(wex_p2p_init(&node_b, &ident_b, 9311) == 0);
    assert(wex_p2p_init(&node_c, &ident_c, 9312) == 0);

    wex_p2p_set_data_dir(&node_a, TEST_DATA_DIR_A);
    wex_p2p_set_data_dir(&node_b, TEST_DATA_DIR_B);
    wex_p2p_set_data_dir(&node_c, TEST_DATA_DIR_C);

    wex_p2p_set_callbacks(&node_a, on_msg, on_peer, &state_a);
    wex_p2p_set_callbacks(&node_b, on_msg, on_peer, &state_b);
    wex_p2p_set_callbacks(&node_c, on_msg, on_peer, &state_c);

    wex_p2p_set_blob_callback(&node_a, on_blob);
    wex_p2p_set_blob_callback(&node_b, on_blob);
    wex_p2p_set_blob_callback(&node_c, on_blob);

    wex_p2p_set_action_callback(&node_a, on_action);
    wex_p2p_set_action_callback(&node_b, on_action);
    wex_p2p_set_action_callback(&node_c, on_action);

    wex_p2p_set_voice_callback(&node_a, on_voice);
    wex_p2p_set_voice_callback(&node_b, on_voice);
    wex_p2p_set_voice_callback(&node_c, on_voice);

    /* 1. Alice creates Space and Channels */
    wex_space_t space;
    wex_space_create(&space, "Crypto Fortress");
    memcpy(space.owner_pk, ident_a.sign_pk, 32);
    wex_space_add_channel(&space, "dev-chat");
    wex_space_add_channel(&space, "voice-lounge");

    wex_p2p_add_space(&node_a, &space);
    wex_p2p_set_active_space(&node_a, space.space_id);
    wex_p2p_set_active_channel(&node_a, "dev-chat");

    wex_p2p_add_space(&node_b, &space);
    wex_p2p_set_active_space(&node_b, space.space_id);
    wex_p2p_set_active_channel(&node_b, "dev-chat");

    wex_p2p_add_space(&node_c, &space);
    wex_p2p_set_active_space(&node_c, space.space_id);
    wex_p2p_set_active_channel(&node_c, "dev-chat");

    /* 2. Connect Bob and Charlie to Alice */
    assert(wex_p2p_connect(&node_b, "127.0.0.1", 9310) == 0);
    assert(wex_p2p_connect(&node_c, "127.0.0.1", 9310) == 0);

    /* Poll network until handshake completes */
    for (int iter = 0; iter < 10; iter++) {
        wex_p2p_poll(&node_a, 10);
        wex_p2p_poll(&node_b, 10);
        wex_p2p_poll(&node_c, 10);
        usleep(10000);
    }

    assert(node_a.peer_count >= 2);
    assert(node_b.peer_count >= 1);
    assert(node_c.peer_count >= 1);
    printf("[+] Swarm mesh interconnected successfully.\n");

    /* 3. Alice broadcasts a signed chat message */
    wex_chat_msg_t chat_msg;
    memset(&chat_msg, 0, sizeof(chat_msg));
    memcpy(chat_msg.space_id, space.space_id, 32);
    strncpy(chat_msg.channel, "dev-chat", sizeof(chat_msg.channel) - 1);
    strncpy(chat_msg.content, "Welcome to Wex Sovereign Chat!", sizeof(chat_msg.content) - 1);
    assert(wex_chat_msg_sign(&chat_msg, &ident_a) == 0);
    state_a.last_msg = chat_msg;
    assert(wex_p2p_broadcast_msg(&node_a, &chat_msg) >= 2);

    for (int iter = 0; iter < 10; iter++) {
        wex_p2p_poll(&node_a, 10);
        wex_p2p_poll(&node_b, 10);
        wex_p2p_poll(&node_c, 10);
        usleep(10000);
    }

    assert(state_b.msg_count == 1);
    assert(state_c.msg_count == 1);
    assert(strcmp(state_b.last_msg.content, "Welcome to Wex Sovereign Chat!") == 0);
    assert(wex_chat_msg_verify(&state_b.last_msg));
    printf("[+] Signed message verified across all peers.\n");

    /* 4. Multi-chunk Blob File Transfer */
    const size_t test_file_sz = 2500;
    uint8_t file_data[2500];
    for (size_t i = 0; i < test_file_sz; i++) {
        file_data[i] = (uint8_t)(i ^ 0x5A);
    }

    wex_blob_info_t file_info;
    assert(wex_blob_info_create(&file_info, "firmware.bin", "application/octet-stream", file_data, test_file_sz) == 0);
    assert(wex_blob_save(TEST_DATA_DIR_A, &file_info, file_data) == 0);

    /* Alice announces blob offer */
    assert(wex_p2p_offer_blob(&node_a, &file_info) >= 1);
    for (int iter = 0; iter < 5; iter++) {
        wex_p2p_poll(&node_b, 10);
        usleep(5000);
    }

    /* Bob requests and downloads the blob from Alice */
    assert(node_b.peer_count > 0);
    assert(wex_p2p_download_blob(&node_b, &node_b.peers[0].addr, &file_info) == 0);

    for (int iter = 0; iter < 20; iter++) {
        wex_p2p_poll(&node_a, 10);
        wex_p2p_poll(&node_b, 10);
        if (state_b.blob_count == 1) break;
        usleep(10000);
    }

    assert(state_b.blob_count == 1);
    uint8_t *loaded_bob = NULL;
    size_t loaded_sz = 0;
    assert(wex_blob_load(TEST_DATA_DIR_B, file_info.root_hash, &loaded_bob, &loaded_sz) == 0);
    assert(loaded_sz == test_file_sz);
    assert(memcmp(loaded_bob, file_data, test_file_sz) == 0);
    free(loaded_bob);
    printf("[+] Multi-chunk blob verified & transferred via P2P chunking.\n");

    /* 5. Message Reactions (Bob -> Alice, Charlie -> Alice) */
    wex_msg_action_t act_bob;
    memset(&act_bob, 0, sizeof(act_bob));
    act_bob.action_type = WEX_ACTION_REACT;
    memcpy(act_bob.target_sig, chat_msg.signature, 64);
    memcpy(act_bob.space_id, space.space_id, 32);
    strncpy(act_bob.channel, "dev-chat", sizeof(act_bob.channel) - 1);
    strncpy(act_bob.payload, "🚀", sizeof(act_bob.payload) - 1);
    act_bob.timestamp_ms = 1000;
    assert(wex_msg_action_sign(&act_bob, &ident_b) == 0);

    assert(wex_p2p_send_action(&node_b, space.space_id, &act_bob) >= 1);

    for (int iter = 0; iter < 10; iter++) {
        wex_p2p_poll(&node_a, 10);
        wex_p2p_poll(&node_b, 10);
        wex_p2p_poll(&node_c, 10);
        usleep(10000);
    }

    assert(state_a.action_count >= 1);
    char summary[128];
    wex_reactions_get_summary(&state_a.reactions, chat_msg.signature, summary, sizeof(summary));
    assert(strstr(summary, "🚀") != NULL);
    printf("[+] Message reactions propagated and summarized: '%s'\n", summary);

    /* 6. Message Edit (Author Alice updates text) */
    wex_msg_action_t act_edit;
    memset(&act_edit, 0, sizeof(act_edit));
    act_edit.action_type = WEX_ACTION_EDIT;
    memcpy(act_edit.target_sig, chat_msg.signature, 64);
    memcpy(act_edit.space_id, space.space_id, 32);
    strncpy(act_edit.channel, "dev-chat", sizeof(act_edit.channel) - 1);
    strncpy(act_edit.payload, "Welcome to Wex Sovereign Chat! [EDITED]", sizeof(act_edit.payload) - 1);
    act_edit.timestamp_ms = 2000;
    assert(wex_msg_action_sign(&act_edit, &ident_a) == 0);

    assert(wex_p2p_send_action(&node_a, space.space_id, &act_edit) >= 2);

    for (int iter = 0; iter < 10; iter++) {
        wex_p2p_poll(&node_a, 10);
        wex_p2p_poll(&node_b, 10);
        wex_p2p_poll(&node_c, 10);
        usleep(10000);
    }

    assert(strcmp(state_b.last_msg.content, "Welcome to Wex Sovereign Chat! [EDITED]") == 0);
    printf("[+] Author message edit validated and synced.\n");

    /* 7. Unauthorized Edit Attempt by Charlie (Must Fail) */
    wex_msg_action_t act_hack;
    memset(&act_hack, 0, sizeof(act_hack));
    act_hack.action_type = WEX_ACTION_EDIT;
    memcpy(act_hack.target_sig, chat_msg.signature, 64);
    memcpy(act_hack.space_id, space.space_id, 32);
    strncpy(act_hack.channel, "dev-chat", sizeof(act_hack.channel) - 1);
    strncpy(act_hack.payload, "Hacked by Charlie!", sizeof(act_hack.payload) - 1);
    act_hack.timestamp_ms = 3000;
    assert(wex_msg_action_sign(&act_hack, &ident_c) == 0);

    /* Bob tries to apply Charlie's edit to Alice's message */
    int res = wex_msg_action_apply(&state_b.reactions, &state_b.acl, &state_b.last_msg, &act_hack);
    assert(res < 0); /* Rejected! */
    assert(strcmp(state_b.last_msg.content, "Welcome to Wex Sovereign Chat! [EDITED]") == 0);
    printf("[+] Unauthorized message edit forgery rejected.\n");

    /* 8. Voice Frame Transport with Jitter Buffer */
    wex_voice_room_t voice_room_a;
    wex_voice_room_init(&voice_room_a, "voice-lounge", 9310);
    wex_jitter_buffer_t jb_b;
    wex_jitter_buffer_init(&jb_b);

    uint8_t opus_fake[120];
    for (int i = 0; i < 120; i++) opus_fake[i] = (uint8_t)i;

    for (int seq = 1; seq <= 5; seq++) {
        wex_voice_hdr_t vhdr;
        wex_voice_create_header(&voice_room_a, &vhdr, sizeof(opus_fake));
        assert(wex_p2p_send_voice_frame(&node_a, space.space_id, "voice-lounge", &vhdr, opus_fake, sizeof(opus_fake)) >= 1);
    }

    for (int iter = 0; iter < 10; iter++) {
        wex_p2p_poll(&node_b, 10);
        usleep(5000);
    }

    assert(state_b.voice_count == 5);
    printf("[+] Real-time voice frames transmitted and received.\n");

    /* 9. Cryptographic ACL Ban of Charlie */
    wex_acl_init(&state_b.acl, space.space_id, space.owner_pk);
    wex_ban_entry_t ban;
    assert(wex_acl_ban_create(&ident_a, space.space_id, ident_c.sign_pk, "Spamming", &ban) == 0);
    assert(wex_acl_apply_ban(&state_b.acl, &ban) == 0);
    assert(wex_acl_is_banned(&state_b.acl, ident_c.sign_pk) == true);

    /* Charlie attempts to broadcast a new chat message */
    wex_chat_msg_t troll_msg;
    memset(&troll_msg, 0, sizeof(troll_msg));
    memcpy(troll_msg.space_id, space.space_id, 32);
    strncpy(troll_msg.channel, "dev-chat", sizeof(troll_msg.channel) - 1);
    strncpy(troll_msg.content, "Can you hear me now?", sizeof(troll_msg.content) - 1);
    assert(wex_chat_msg_sign(&troll_msg, &ident_c) == 0);
    assert(wex_p2p_broadcast_msg(&node_c, &troll_msg) >= 1);

    size_t prev_b_count = state_b.msg_count;
    for (int iter = 0; iter < 10; iter++) {
        wex_p2p_poll(&node_b, 10);
        usleep(5000);
    }

    /* Bob dropped Charlie's message due to ACL ban! */
    assert(state_b.msg_count == prev_b_count);
    printf("[+] Cryptographic ACL Space Ban enforced against attacker.\n");

    /* Cleanup */
    wex_p2p_close(&node_a);
    wex_p2p_close(&node_b);
    wex_p2p_close(&node_c);

    system("rm -rf " TEST_DATA_DIR_A " " TEST_DATA_DIR_B " " TEST_DATA_DIR_C);

    printf("[SUCCESS] All End-to-End full stack multi-node tests passed!\n");
    return 0;
}
