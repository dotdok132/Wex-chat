#define _GNU_SOURCE
#include "wex_persist.h"
#include "wex_p2p.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <arpa/inet.h>

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

static void test_peers_persistence(void) {
    const char *test_dir = "/tmp/wex_test_contacts_dir";
    const char *contacts_path = "/tmp/wex_test_contacts_dir/contacts.dat";
    unlink(contacts_path);
    wex_mkdir_p(test_dir);

    wex_identity_t self_id, peer1_id, peer2_id;
    assert(wex_identity_init(&self_id, NULL, "Self_Node") == 0);
    assert(wex_identity_init(&peer1_id, NULL, "LAN_Friend") == 0);
    assert(wex_identity_init(&peer2_id, NULL, "Roaming_Friend") == 0);

    /* Initialize node 1 and populate discovered peers (LAN + WAN roaming endpoints) */
    wex_p2p_node_t node1;
    assert(wex_p2p_init(&node1, &self_id, 0) == 0);

    node1.peer_count = 2;

    /* Peer 1: LAN endpoint only */
    memset(&node1.peers[0], 0, sizeof(wex_peer_t));
    memcpy(node1.peers[0].pk, peer1_id.sign_pk, 32);
    memcpy(node1.peers[0].x25519_pk, peer1_id.x25519_pk, 32);
    strncpy(node1.peers[0].nickname, peer1_id.nickname, WEX_NAME_LEN - 1);
    node1.peers[0].addr.sin_family = AF_INET;
    inet_pton(AF_INET, "192.168.1.42", &node1.peers[0].addr.sin_addr);
    node1.peers[0].addr.sin_port = htons(9001);
    node1.peers[0].has_wan_addr = false;
    node1.peers[0].active = true;

    /* Peer 2: Both LAN addr and discovered WAN addr for LAN-to-WAN roaming */
    memset(&node1.peers[1], 0, sizeof(wex_peer_t));
    memcpy(node1.peers[1].pk, peer2_id.sign_pk, 32);
    memcpy(node1.peers[1].x25519_pk, peer2_id.x25519_pk, 32);
    strncpy(node1.peers[1].nickname, peer2_id.nickname, WEX_NAME_LEN - 1);
    node1.peers[1].addr.sin_family = AF_INET;
    inet_pton(AF_INET, "192.168.1.99", &node1.peers[1].addr.sin_addr);
    node1.peers[1].addr.sin_port = htons(9002);
    node1.peers[1].has_wan_addr = true;
    node1.peers[1].wan_addr.sin_family = AF_INET;
    inet_pton(AF_INET, "203.0.113.55", &node1.peers[1].wan_addr.sin_addr);
    node1.peers[1].wan_addr.sin_port = htons(45000);
    node1.peers[1].active = true;

    /* Save contacts to disk */
    assert(wex_peers_save(&node1, contacts_path) == 0);
    wex_p2p_close(&node1);

    /* Simulate node restart: initialize fresh node2 and restore via wex_p2p_set_data_dir */
    wex_p2p_node_t node2;
    assert(wex_p2p_init(&node2, &self_id, 0) == 0);
    assert(node2.peer_count == 0);

    wex_p2p_set_data_dir(&node2, test_dir);
    assert(node2.peer_count == 2);

    /* Verify Peer 1 fields */
    assert(node2.peers[0].active == true);
    assert(memcmp(node2.peers[0].pk, peer1_id.sign_pk, 32) == 0);
    assert(memcmp(node2.peers[0].x25519_pk, peer1_id.x25519_pk, 32) == 0);
    assert(strcmp(node2.peers[0].nickname, "LAN_Friend") == 0);
    assert(node2.peers[0].addr.sin_port == htons(9001));
    char ip_buf[32];
    inet_ntop(AF_INET, &node2.peers[0].addr.sin_addr, ip_buf, sizeof(ip_buf));
    assert(strcmp(ip_buf, "192.168.1.42") == 0);
    assert(node2.peers[0].has_wan_addr == false);

    /* Verify Peer 2 fields including WAN roaming endpoint */
    assert(node2.peers[1].active == true);
    assert(memcmp(node2.peers[1].pk, peer2_id.sign_pk, 32) == 0);
    assert(memcmp(node2.peers[1].x25519_pk, peer2_id.x25519_pk, 32) == 0);
    assert(strcmp(node2.peers[1].nickname, "Roaming_Friend") == 0);
    assert(node2.peers[1].addr.sin_port == htons(9002));
    inet_ntop(AF_INET, &node2.peers[1].addr.sin_addr, ip_buf, sizeof(ip_buf));
    assert(strcmp(ip_buf, "192.168.1.99") == 0);
    assert(node2.peers[1].has_wan_addr == true);
    assert(node2.peers[1].wan_addr.sin_port == htons(45000));
    inet_ntop(AF_INET, &node2.peers[1].wan_addr.sin_addr, ip_buf, sizeof(ip_buf));
    assert(strcmp(ip_buf, "203.0.113.55") == 0);

    /* Verify DHT routing table was also populated with restored peers */
    assert(node2.dht.total_contacts >= 2);

    wex_p2p_close(&node2);
    unlink(contacts_path);
    rmdir(test_dir);

    TEST_PASS("Peer Contacts (LAN + WAN Roaming Endpoints) Persistence Across Restarts");
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex Persistence & Storage Tests           \n");
    printf("====================================================\n");

    test_identity_persistence();
    test_journal_persistence();
    test_peers_persistence();

    printf("====================================================\n");
    printf("  ALL PERSISTENCE TESTS PASSED SUCCESSFULLY!        \n");
    printf("====================================================\n");
    return 0;
}
