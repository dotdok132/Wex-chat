#define _GNU_SOURCE
#include "wex_crypto.h"
#include "wex_models.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

static void test_x25519_ecdh_and_kdf(void) {
    wex_identity_t alice, bob;
    assert(wex_identity_init(&alice, NULL, "Alice") == 0);
    assert(wex_identity_init(&bob, NULL, "Bob") == 0);

    /* 1. Both derive shared secret via X25519 ECDH */
    uint8_t secret_a[32], secret_b[32];
    assert(wex_ecdh(alice.x25519_sk, bob.x25519_pk, secret_a) == 0);
    assert(wex_ecdh(bob.x25519_sk, alice.x25519_pk, secret_b) == 0);

    assert(memcmp(secret_a, secret_b, 32) == 0);

    /* 2. Derive directional session keys via BLAKE2b KDF */
    uint8_t key_ab[32], key_ba[32];
    uint8_t salt[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    assert(wex_kdf_session_key(secret_a, salt, "WEX_SESSION_A_TO_B", key_ab) == 0);
    assert(wex_kdf_session_key(secret_b, salt, "WEX_SESSION_A_TO_B", key_ba) == 0);

    assert(memcmp(key_ab, key_ba, 32) == 0);

    TEST_PASS("X25519 Diffie-Hellman Key Exchange and BLAKE2b KDF Session Key Derivation");
}

static void test_channel_e2ee_encryption(void) {
    wex_identity_t alice, bob, eve;
    assert(wex_identity_init(&alice, NULL, "Alice") == 0);
    assert(wex_identity_init(&bob, NULL, "Bob") == 0);
    assert(wex_identity_init(&eve, NULL, "Eve") == 0);

    wex_space_t space;
    assert(wex_space_create(&space, "SovereignGuild") == 0);

    wex_chat_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    memcpy(msg.space_id, space.space_id, 32);
    strncpy(msg.channel, "general", 31);
    strncpy(msg.content, "Confidential roadmap for Wex decentralized Discord", sizeof(msg.content) - 1);

    /* Alice encrypts message with channel space_key */
    assert(wex_chat_msg_encrypt(&msg, space.space_key, 1) == 0);
    assert(msg.enc_flags == 1);
    assert(strstr(msg.content, "Confidential") == NULL); /* Plaintext is not exposed */

    /* Alice signs message */
    assert(wex_chat_msg_sign(&msg, &alice) == 0);

    /* Eve (eavesdropper) intercepts message: signature is valid, but Eve cannot decrypt */
    assert(wex_chat_msg_verify(&msg) == true);
    uint8_t wrong_key[32];
    memset(wrong_key, 0xFF, 32);
    wex_chat_msg_t msg_copy = msg;
    assert(wex_chat_msg_decrypt(&msg_copy, wrong_key) != 0);

    /* Bob has legitimate space_key: decrypts successfully */
    assert(wex_chat_msg_decrypt(&msg, space.space_key) == 0);
    assert(msg.enc_flags == 0);
    assert(strcmp(msg.content, "Confidential roadmap for Wex decentralized Discord") == 0);

    TEST_PASS("Space & Channel E2EE Encryption with ChaCha20-Poly1305 and Ed25519 Signature");
}

static void test_direct_message_e2ee(void) {
    wex_identity_t alice, bob;
    assert(wex_identity_init(&alice, NULL, "Alice") == 0);
    assert(wex_identity_init(&bob, NULL, "Bob") == 0);

    /* Derive pairwise DM session key */
    uint8_t secret[32];
    assert(wex_ecdh(alice.x25519_sk, bob.x25519_pk, secret) == 0);
    uint8_t dm_key[32];
    assert(wex_kdf_session_key(secret, NULL, "WEX_DM_SESSION", dm_key) == 0);

    wex_chat_msg_t dm;
    memset(&dm, 0, sizeof(dm));
    strncpy(dm.channel, "dm", 31);
    strncpy(dm.content, "Hello Bob, this is a private direct message!", sizeof(dm.content) - 1);

    assert(wex_chat_msg_encrypt(&dm, dm_key, 2) == 0);
    assert(dm.enc_flags == 2);
    assert(wex_chat_msg_sign(&dm, &alice) == 0);

    /* Bob receives and decrypts */
    assert(wex_chat_msg_verify(&dm) == true);
    assert(wex_chat_msg_decrypt(&dm, dm_key) == 0);
    assert(dm.enc_flags == 0);
    assert(strcmp(dm.content, "Hello Bob, this is a private direct message!") == 0);

    TEST_PASS("Direct Messaging (DM) Pairwise E2EE Encryption");
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex E2EE Session & Encryption Tests       \n");
    printf("====================================================\n");

    test_x25519_ecdh_and_kdf();
    test_channel_e2ee_encryption();
    test_direct_message_e2ee();

    printf("====================================================\n");
    printf("  ALL E2EE TESTS PASSED SUCCESSFULLY!               \n");
    printf("====================================================\n");
    return 0;
}
