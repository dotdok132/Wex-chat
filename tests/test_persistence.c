#define _GNU_SOURCE
#include "wex_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

static void test_identity_persistence(void) {
    const char *key_path = "/tmp/wex_test_ident.key";
    unlink(key_path);

    wex_identity_t id1;
    assert(wex_identity_init(&id1, NULL, "Alice_Persistent") == 0);

    /* 1. Save identity */
    assert(wex_identity_save(&id1, key_path) == 0);

    /* 2. Load identity into id2 */
    wex_identity_t id2;
    memset(&id2, 0, sizeof(id2));
    assert(wex_identity_load(&id2, key_path) == 0);

    assert(memcmp(id1.seed, id2.seed, 32) == 0);
    assert(memcmp(id1.sign_pk, id2.sign_pk, 32) == 0);
    assert(memcmp(id1.sign_sk, id2.sign_sk, 64) == 0);
    assert(memcmp(id1.x25519_pk, id2.x25519_pk, 32) == 0);
    assert(memcmp(id1.x25519_sk, id2.x25519_sk, 32) == 0);
    assert(strcmp(id1.nickname, id2.nickname) == 0);

    /* 3. Tamper detection: flip 1 byte in file */
    FILE *f = fopen(key_path, "r+b");
    assert(f != NULL);
    fseek(f, 20, SEEK_SET);
    int c = fgetc(f);
    fseek(f, 20, SEEK_SET);
    fputc(c ^ 0xFF, f);
    fclose(f);

    wex_identity_t id_tampered;
    assert(wex_identity_load(&id_tampered, key_path) != 0); /* Must fail checksum */

    unlink(key_path);

    /* 4. Test load_or_create */
    wex_identity_t id3;
    assert(wex_identity_load_or_create(&id3, key_path, "Bob_Auto") == 0);
    assert(strcmp(id3.nickname, "Bob_Auto") == 0);

    wex_identity_t id4;
    assert(wex_identity_load_or_create(&id4, key_path, "Different_Nick") == 0);
    assert(memcmp(id3.sign_pk, id4.sign_pk, 32) == 0); /* Loaded existing */
    assert(strcmp(id4.nickname, "Bob_Auto") == 0);

    unlink(key_path);
    TEST_PASS("Cryptographic Identity Save/Load/Tamper-detection");
}

static void test_journal_persistence(void) {
    const char *journal_path = "/tmp/wex_test_journal.log";
    unlink(journal_path);

    wex_journal_t *j = NULL;
    assert(wex_journal_open(&j, journal_path) == 0);
    assert(j != NULL);
    assert(wex_journal_count(j) == 0);

    uint8_t space_a[32], space_b[32];
    memset(space_a, 0x11, 32);
    memset(space_b, 0x22, 32);

    /* Append 5 messages */
    for (int i = 0; i < 5; i++) {
        wex_chat_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        memcpy(msg.space_id, (i % 2 == 0) ? space_a : space_b, 32);
        strncpy(msg.channel, (i == 4) ? "dev" : "general", 31);
        snprintf(msg.author, sizeof(msg.author), "User%d", i);
        snprintf(msg.content, sizeof(msg.content), "Message number %d in journal", i);
        msg.timestamp_ms = 1000 + i;
        assert(wex_journal_append(j, &msg) == 0);
    }

    assert(wex_journal_count(j) == 5);
    wex_journal_close(j);

    /* Reopen journal and check persistence */
    wex_journal_t *j2 = NULL;
    assert(wex_journal_open(&j2, journal_path) == 0);
    assert(wex_journal_count(j2) == 5);

    /* Read messages for space_a in "general" (should match i=0, i=2) */
    wex_chat_msg_t recent[10];
    size_t count = wex_journal_read_recent(j2, space_a, "general", recent, 10);
    assert(count == 2);
    assert(strcmp(recent[0].content, "Message number 0 in journal") == 0);
    assert(strcmp(recent[1].content, "Message number 2 in journal") == 0);

    /* Read all messages in space_b */
    count = wex_journal_read_recent(j2, space_b, NULL, recent, 10);
    assert(count == 2); /* i=1, i=3 */

    wex_journal_close(j2);
    unlink(journal_path);

    TEST_PASS("Append-only Binary Message Journal Persistence & Querying");
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex Persistence & Storage Tests           \n");
    printf("====================================================\n");

    test_identity_persistence();
    test_journal_persistence();

    printf("====================================================\n");
    printf("  ALL PERSISTENCE TESTS PASSED SUCCESSFULLY!        \n");
    printf("====================================================\n");
    return 0;
}
