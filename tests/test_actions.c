#define _GNU_SOURCE
#include "wex_actions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

static void test_message_edits_and_deletions(void) {
    wex_identity_t owner, alice, bob, eve;
    assert(wex_identity_init(&owner, NULL, "GuildOwner") == 0);
    assert(wex_identity_init(&alice, NULL, "Alice") == 0);
    assert(wex_identity_init(&bob, NULL, "Bob") == 0);
    assert(wex_identity_init(&eve, NULL, "Eve") == 0);

    uint8_t space_id[32] = {0x01};
    wex_acl_t acl;
    wex_acl_init(&acl, space_id, owner.sign_pk);

    /* Alice creates and signs a message */
    wex_chat_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    memcpy(msg.space_id, space_id, 32);
    strncpy(msg.channel, "general", sizeof(msg.channel) - 1);
    strncpy(msg.content, "Original message with a typo here", sizeof(msg.content) - 1);
    assert(wex_chat_msg_sign(&msg, &alice) == 0);

    /* 1. Bob tries to edit Alice's message */
    wex_msg_action_t edit_by_bob = {
        .action_type = WEX_ACTION_EDIT,
        .timestamp_ms = 1000
    };
    memcpy(edit_by_bob.target_sig, msg.signature, 64);
    memcpy(edit_by_bob.space_id, space_id, 32);
    strncpy(edit_by_bob.channel, "general", sizeof(edit_by_bob.channel) - 1);
    strncpy(edit_by_bob.payload, "Bob hacked your message!", sizeof(edit_by_bob.payload) - 1);
    assert(wex_msg_action_sign(&edit_by_bob, &bob) == 0);

    assert(wex_msg_action_apply(NULL, &acl, &msg, &edit_by_bob) == -4); /* Permission denied */
    assert(strcmp(msg.content, "Original message with a typo here") == 0);

    /* 2. Alice legitimately edits her message */
    wex_msg_action_t edit_by_alice = {
        .action_type = WEX_ACTION_EDIT,
        .timestamp_ms = 1050
    };
    memcpy(edit_by_alice.target_sig, msg.signature, 64);
    memcpy(edit_by_alice.space_id, space_id, 32);
    strncpy(edit_by_alice.channel, "general", sizeof(edit_by_alice.channel) - 1);
    strncpy(edit_by_alice.payload, "Original message corrected without typos", sizeof(edit_by_alice.payload) - 1);
    assert(wex_msg_action_sign(&edit_by_alice, &alice) == 0);

    assert(wex_msg_action_apply(NULL, &acl, &msg, &edit_by_alice) == 0);
    assert(strcmp(msg.content, "Original message corrected without typos") == 0);

    /* 3. Eve tries to delete Alice's message */
    wex_msg_action_t del_by_eve = {
        .action_type = WEX_ACTION_DELETE,
        .timestamp_ms = 1100
    };
    memcpy(del_by_eve.target_sig, msg.signature, 64);
    memcpy(del_by_eve.space_id, space_id, 32);
    strncpy(del_by_eve.channel, "general", sizeof(del_by_eve.channel) - 1);
    assert(wex_msg_action_sign(&del_by_eve, &eve) == 0);

    assert(wex_msg_action_apply(NULL, &acl, &msg, &del_by_eve) == -5); /* Unauthorized */

    /* 4. Space Owner (Admin) deletes message */
    wex_msg_action_t del_by_owner = {
        .action_type = WEX_ACTION_DELETE,
        .timestamp_ms = 1200
    };
    memcpy(del_by_owner.target_sig, msg.signature, 64);
    memcpy(del_by_owner.space_id, space_id, 32);
    strncpy(del_by_owner.channel, "general", sizeof(del_by_owner.channel) - 1);
    assert(wex_msg_action_sign(&del_by_owner, &owner) == 0);

    assert(wex_msg_action_apply(NULL, &acl, &msg, &del_by_owner) == 0);
    assert(strcmp(msg.content, "[Message deleted]") == 0);

    TEST_PASS("Cryptographically Signed Message Edits & Admin/Author Deletions");
}

static void test_message_emoji_reactions(void) {
    wex_identity_t alice, bob, charlie;
    assert(wex_identity_init(&alice, NULL, "Alice") == 0);
    assert(wex_identity_init(&bob, NULL, "Bob") == 0);
    assert(wex_identity_init(&charlie, NULL, "Charlie") == 0);

    wex_chat_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.content, "Check out Wex-Chat sovereign mesh!", sizeof(msg.content) - 1);
    assert(wex_chat_msg_sign(&msg, &alice) == 0);

    wex_reactions_store_t store;
    wex_reactions_store_init(&store);

    /* Bob reacts with 👍 */
    wex_msg_action_t r1 = { .action_type = WEX_ACTION_REACT, .timestamp_ms = 2000 };
    memcpy(r1.target_sig, msg.signature, 64);
    strncpy(r1.payload, "👍", sizeof(r1.payload) - 1);
    assert(wex_msg_action_sign(&r1, &bob) == 0);
    assert(wex_msg_action_apply(&store, NULL, &msg, &r1) == 0);

    /* Charlie also reacts with 👍 */
    wex_msg_action_t r2 = { .action_type = WEX_ACTION_REACT, .timestamp_ms = 2010 };
    memcpy(r2.target_sig, msg.signature, 64);
    strncpy(r2.payload, "👍", sizeof(r2.payload) - 1);
    assert(wex_msg_action_sign(&r2, &charlie) == 0);
    assert(wex_msg_action_apply(&store, NULL, &msg, &r2) == 0);

    char summary[128];
    assert(wex_reactions_get_summary(&store, msg.signature, summary, sizeof(summary)) == 0);
    assert(strstr(summary, "👍 2") != NULL);

    /* Bob reacts again with 👍 -> toggles reaction off! */
    assert(wex_msg_action_apply(&store, NULL, &msg, &r1) == 0);
    assert(wex_reactions_get_summary(&store, msg.signature, summary, sizeof(summary)) == 0);
    assert(strstr(summary, "👍 1") != NULL);

    /* Bob reacts with 🔥 */
    wex_msg_action_t r3 = { .action_type = WEX_ACTION_REACT, .timestamp_ms = 2050 };
    memcpy(r3.target_sig, msg.signature, 64);
    strncpy(r3.payload, "🔥", sizeof(r3.payload) - 1);
    assert(wex_msg_action_sign(&r3, &bob) == 0);
    assert(wex_msg_action_apply(&store, NULL, &msg, &r3) == 0);

    assert(wex_reactions_get_summary(&store, msg.signature, summary, sizeof(summary)) == 0);
    assert(strstr(summary, "👍 1") != NULL);
    assert(strstr(summary, "🔥 1") != NULL);

    TEST_PASS("Emoji Reactions Toggling, Multi-User Aggregation & Summary");
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex Message Actions & Reactions Tests     \n");
    printf("====================================================\n");

    test_message_edits_and_deletions();
    test_message_emoji_reactions();

    printf("====================================================\n");
    printf("  ALL MESSAGE ACTIONS TESTS PASSED SUCCESSFULLY!    \n");
    printf("====================================================\n");
    return 0;
}
