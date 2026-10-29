#include "wex_models.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

static void test_identity_and_ed25519(void) {
    wex_identity_t ident;
    assert(wex_identity_init(&ident, NULL, "dotdok") == 0);
    assert(strcmp(ident.nickname, "dotdok") == 0);
    assert(strcmp(ident.status, "online") == 0);

    /* Test chat message signing and verification */
    wex_chat_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    memset(msg.space_id, 0x42, WEX_ID_LEN);
    strncpy(msg.channel, "general", sizeof(msg.channel) - 1);
    strncpy(msg.content, "Sovereign P2P Discord Alternative!", sizeof(msg.content) - 1);

    assert(wex_chat_msg_sign(&msg, &ident) == 0);
    assert(wex_chat_msg_verify(&msg) == true);

    /* Tampering with content must invalidate signature */
    msg.content[0] ^= 0x01;
    assert(wex_chat_msg_verify(&msg) == false);

    TEST_PASS("Identity generation & Ed25519 message signing/verification");
}

static void test_spaces_and_invites(void) {
    wex_space_t space;
    assert(wex_space_create(&space, "Cyber Guild") == 0);
    assert(strcmp(space.name, "Cyber Guild") == 0);
    assert(space.channel_count == 2); /* general, dev */

    assert(wex_space_add_channel(&space, "memes") == 0);
    assert(space.channel_count == 3);

    char invite[256];
    assert(wex_space_get_invite(&space, "192.168.1.50:9000", invite, sizeof(invite)) == 0);
    assert(strstr(invite, "wex://join/") == invite);
    assert(strstr(invite, "peer=192.168.1.50:9000") != NULL);
    assert(strstr(invite, "name=Cyber Guild") != NULL);

    TEST_PASS("Spaces, channel management, and wex:// invite link formatting");
}

static void test_hex_conversion(void) {
    uint8_t id[32];
    for (int i = 0; i < 32; i++) {
        id[i] = (uint8_t)i;
    }

    char hex[65];
    wex_id_to_hex(id, hex);
    assert(strlen(hex) == 64);

    uint8_t parsed[32];
    assert(wex_hex_to_id(hex, parsed) == 0);
    assert(memcmp(id, parsed, 32) == 0);

    TEST_PASS("Binary ID <-> Hex conversion helpers");
}

int main(void) {
    printf("==================================================\n");
    printf("  Running Wex Models & Signing Test Suite\n");
    printf("==================================================\n");

    test_identity_and_ed25519();
    test_spaces_and_invites();
    test_hex_conversion();

    printf("==================================================\n");
    printf("  ALL MODELS & SIGNING TESTS PASSED!\n");
    printf("==================================================\n");
    return 0;
}
