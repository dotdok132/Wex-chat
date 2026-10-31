#define _GNU_SOURCE
#include "wex_nat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

static void test_addr_formatting(void) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9001);
    inet_pton(AF_INET, "192.168.1.100", &addr.sin_addr);

    char str[64];
    wex_addr_to_string(&addr, str, sizeof(str));
    assert(strcmp(str, "192.168.1.100:9001") == 0);

    TEST_PASS("Endpoint address to string formatting");
}

static void test_local_mock_stun(void) {
    /* Setup mock STUN responder on localhost UDP */
    int mock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(mock_fd >= 0);

    struct sockaddr_in mock_addr;
    memset(&mock_addr, 0, sizeof(mock_addr));
    mock_addr.sin_family = AF_INET;
    mock_addr.sin_port = htons(29999);
    mock_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(mock_fd, (struct sockaddr *)&mock_addr, sizeof(mock_addr)) == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* Child: mock STUN server process */
        uint8_t in_buf[512];
        struct sockaddr_in client_src;
        socklen_t slen = sizeof(client_src);
        ssize_t n = recvfrom(mock_fd, in_buf, sizeof(in_buf), 0, (struct sockaddr *)&client_src, &slen);
        if (n >= (ssize_t)sizeof(wex_stun_hdr_t)) {
            wex_stun_hdr_t *req = (wex_stun_hdr_t *)in_buf;

            uint8_t resp_buf[128];
            wex_stun_hdr_t *resp_hdr = (wex_stun_hdr_t *)resp_buf;
            resp_hdr->msg_type = htons(0x0101);
            resp_hdr->msg_len = htons(12);
            resp_hdr->magic_cookie = htonl(0x2112A442);
            memcpy(resp_hdr->transaction_id, req->transaction_id, 12);

            /* Attribute XOR-MAPPED-ADDRESS (0x0020), length 8 */
            uint16_t *attr_type = (uint16_t *)(resp_buf + 20);
            uint16_t *attr_len = (uint16_t *)(resp_buf + 22);
            *attr_type = htons(0x0020);
            *attr_len = htons(8);

            uint8_t *val = resp_buf + 24;
            val[0] = 0x00;
            val[1] = 0x01; /* IPv4 */
            /* Mapped port: 45678 ^ 0x2112 */
            uint16_t fake_port = 45678;
            uint16_t xor_port = fake_port ^ (uint16_t)(0x2112A442 >> 16);
            *(uint16_t *)(val + 2) = htons(xor_port);

            /* Mapped IP: 203.0.113.199 ^ 0x2112A442 */
            uint32_t fake_ip;
            inet_pton(AF_INET, "203.0.113.199", &fake_ip);
            *(uint32_t *)(val + 4) = fake_ip ^ htonl(0x2112A442);

            sendto(mock_fd, resp_buf, 20 + 4 + 8, 0, (struct sockaddr *)&client_src, sizeof(client_src));
        }
        close(mock_fd);
        _exit(0);
    }

    /* Parent: Client socket */
    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(client_fd >= 0);

    /* Query STUN via our implementation */
    struct sockaddr_in discovered;
    int res = wex_stun_query(client_fd, "127.0.0.1", 29999, &discovered, 1000);
    assert(res == 0);
    assert(ntohs(discovered.sin_port) == 45678);

    char ip_str[32];
    inet_ntop(AF_INET, &discovered.sin_addr, ip_str, sizeof(ip_str));
    assert(strcmp(ip_str, "203.0.113.199") == 0);

    close(client_fd);
    close(mock_fd);
    waitpid(pid, NULL, 0);

    TEST_PASS("Mock STUN RFC 5389 XOR-MAPPED-ADDRESS resolution");
}

static void test_live_stun_if_available(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;

    struct sockaddr_in external_ep;
    int ret = wex_nat_auto_discover(fd, &external_ep);
    if (ret == 0) {
        char ep_str[64];
        wex_addr_to_string(&external_ep, ep_str, sizeof(ep_str));
        printf("    [*] Live STUN resolved external public endpoint: %s\n", ep_str);
        TEST_PASS("Live STUN Auto-discovery on WAN");
    } else {
        printf("    [*] Note: Live STUN WAN lookup timed out (expected if isolated from Internet)\n");
    }

    close(fd);
}

#include "wex_p2p.h"

typedef struct {
    int received;
    char text[WEX_MAX_MSG_LEN];
} nat_sink_t;

static void on_nat_test_msg(const wex_chat_msg_t *msg, void *user_data) {
    nat_sink_t *s = (nat_sink_t *)user_data;
    s->received++;
    strncpy(s->text, msg->content, sizeof(s->text) - 1);
}

static void test_coordinated_nat_hole_punch(void) {
    wex_identity_t id_r, id_a, id_b;
    assert(wex_identity_init(&id_r, NULL, "Rendezvous") == 0);
    assert(wex_identity_init(&id_a, NULL, "Alice_BehindNAT") == 0);
    assert(wex_identity_init(&id_b, NULL, "Bob_BehindNAT") == 0);

    wex_p2p_node_t node_r, node_a, node_b;
    assert(wex_p2p_init(&node_r, &id_r, 31001) == 0);
    assert(wex_p2p_init(&node_a, &id_a, 31002) == 0);
    assert(wex_p2p_init(&node_b, &id_b, 31003) == 0);

    nat_sink_t sink_b = {0};
    wex_p2p_set_callbacks(&node_b, on_nat_test_msg, NULL, &sink_b);

    /* Both Alice and Bob connect to public rendezvous R */
    assert(wex_p2p_connect(&node_a, "127.0.0.1", 31001) == 0);
    assert(wex_p2p_connect(&node_b, "127.0.0.1", 31001) == 0);

    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&node_r, 5);
        wex_p2p_poll(&node_a, 5);
        wex_p2p_poll(&node_b, 5);
        usleep(5000);
    }

    assert(node_r.peer_count == 2);
    /* Alice does NOT have Bob in peers list */
    assert(node_a.peer_count == 1);

    /* Alice requests rendezvous R to coordinate hole punch to Bob */
    struct sockaddr_in r_addr;
    memset(&r_addr, 0, sizeof(r_addr));
    r_addr.sin_family = AF_INET;
    r_addr.sin_port = htons(31001);
    inet_pton(AF_INET, "127.0.0.1", &r_addr.sin_addr);

    assert(wex_p2p_punch_hole(&node_a, &r_addr, id_b.sign_pk) == 0);

    /* Poll network for hole punching exchange */
    for (int t = 0; t < 25; t++) {
        wex_p2p_poll(&node_r, 5);
        wex_p2p_poll(&node_a, 5);
        wex_p2p_poll(&node_b, 5);
        usleep(5000);
    }

    /* Alice and Bob must now have direct peer sessions established */
    bool a_knows_b = false;
    for (size_t i = 0; i < node_a.peer_count; i++) {
        if (memcmp(node_a.peers[i].pk, id_b.sign_pk, 32) == 0) {
            a_knows_b = true;
            break;
        }
    }
    assert(a_knows_b == true);

    /* Alice sends direct chat message to Bob through the punched UDP channel */
    wex_chat_msg_t chat;
    memset(&chat, 0, sizeof(chat));
    strncpy(chat.channel, "general", 31);
    strncpy(chat.content, "Direct P2P UDP packet traversed NAT hole successfully!", WEX_MAX_MSG_LEN - 1);
    assert(wex_chat_msg_sign(&chat, &id_a) == 0);

    assert(wex_p2p_broadcast_msg(&node_a, &chat) >= 1);

    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&node_b, 5);
        usleep(5000);
    }

    assert(sink_b.received == 1);
    assert(strcmp(sink_b.text, "Direct P2P UDP packet traversed NAT hole successfully!") == 0);

    TEST_PASS("Coordinated UDP NAT Hole Punching (A <-> Rendezvous <-> B -> Direct P2P Messaging)");

    wex_p2p_close(&node_r);
    wex_p2p_close(&node_a);
    wex_p2p_close(&node_b);
}

static void test_symmetric_nat_relay_fallback(void) {
    wex_identity_t id_r, id_a, id_b;
    assert(wex_identity_init(&id_r, NULL, "Seed_Relay") == 0);
    assert(wex_identity_init(&id_a, NULL, "Alice_SymmetricNAT") == 0);
    assert(wex_identity_init(&id_b, NULL, "Bob_SymmetricNAT") == 0);

    wex_p2p_node_t node_r, node_a, node_b;
    assert(wex_p2p_init(&node_r, &id_r, 31101) == 0);
    assert(wex_p2p_init(&node_a, &id_a, 31102) == 0);
    assert(wex_p2p_init(&node_b, &id_b, 31103) == 0);

    nat_sink_t sink_b = {0};
    wex_p2p_set_callbacks(&node_b, on_nat_test_msg, NULL, &sink_b);

    /* Both connect to Seed/Relay */
    assert(wex_p2p_connect(&node_a, "127.0.0.1", 31101) == 0);
    assert(wex_p2p_connect(&node_b, "127.0.0.1", 31101) == 0);

    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&node_r, 5);
        wex_p2p_poll(&node_a, 5);
        wex_p2p_poll(&node_b, 5);
        usleep(3000);
    }

    struct sockaddr_in r_addr;
    memset(&r_addr, 0, sizeof(r_addr));
    r_addr.sin_family = AF_INET;
    r_addr.sin_port = htons(31101);
    inet_pton(AF_INET, "127.0.0.1", &r_addr.sin_addr);

    /* Alice initiates punch to Bob */
    assert(wex_p2p_punch_hole(&node_a, &r_addr, id_b.sign_pk) == 0);
    assert(node_a.punch_tracker_count == 1);

    /* Simulate 3.5s timeout where direct UDP was blocked by Symmetric NAT firewall */
    wex_p2p_maintain(&node_a, node_a.punch_trackers[0].punch_started_ms + 3500);

    /* Automated Relay Fallback should be triggered */
    assert(node_a.punch_trackers[0].relay_fallback == true);

    bool b_relayed = false;
    for (size_t i = 0; i < node_a.peer_count; i++) {
        if (memcmp(node_a.peers[i].pk, id_b.sign_pk, 32) == 0) {
            assert(node_a.peers[i].use_relay == true);
            b_relayed = true;
            break;
        }
    }
    assert(b_relayed == true);

    /* Alice sends message to Bob - should seamlessly route via Seed/Relay */
    wex_chat_msg_t chat;
    memset(&chat, 0, sizeof(chat));
    strncpy(chat.channel, "general", 31);
    strncpy(chat.content, "Automated Symmetric NAT Relay Fallback succeeded!", WEX_MAX_MSG_LEN - 1);
    assert(wex_chat_msg_sign(&chat, &id_a) == 0);

    assert(wex_p2p_broadcast_msg(&node_a, &chat) >= 1);

    for (int t = 0; t < 20; t++) {
        wex_p2p_poll(&node_r, 5);
        wex_p2p_poll(&node_b, 5);
        usleep(3000);
    }

    assert(sink_b.received == 1);
    assert(strcmp(sink_b.text, "Automated Symmetric NAT Relay Fallback succeeded!") == 0);

    wex_p2p_close(&node_r);
    wex_p2p_close(&node_a);
    wex_p2p_close(&node_b);

    TEST_PASS("Automated Symmetric NAT Relay Fallback (timeout -> seamless multi-hop relay)");
}

static void test_multi_endpoint_invite_and_spraying(void) {
    wex_identity_t id_host, id_client;
    assert(wex_identity_init(&id_host, NULL, "HostNode") == 0);
    assert(wex_identity_init(&id_client, NULL, "ClientNode") == 0);

    wex_p2p_node_t host, client;
    assert(wex_p2p_init(&host, &id_host, 31201) == 0);
    assert(wex_p2p_init(&client, &id_client, 31202) == 0);

    /* Simulate resolved external WAN endpoint via STUN on host */
    host.has_external_addr = true;
    host.external_addr.sin_family = AF_INET;
    host.external_addr.sin_port = htons(55555);
    inet_pton(AF_INET, "198.51.100.42", &host.external_addr.sin_addr);

    char invite_uri[512];
    assert(wex_p2p_create_invite(&host, invite_uri, sizeof(invite_uri)) == 0);

    /* Ensure invite includes both wan and lan endpoints */
    assert(strstr(invite_uri, "wan=198.51.100.42:55555") != NULL);
    assert(strstr(invite_uri, "lan=") != NULL);
    assert(strstr(invite_uri, ":31201") != NULL);

    /* Test client joining multi-endpoint invite URI */
    /* Note: WAN 198.51.100.42:55555 is not reachable, but LAN 127.0.0.1:31201 succeeds simultaneously */
    char test_uri[512];
    snprintf(test_uri, sizeof(test_uri),
             "wex://join/0000000000000000000000000000000000000000000000000000000000000000?wan=198.51.100.42:55555&lan=127.0.0.1:31201&pk=");
    char pk_hex[65];
    wex_id_to_hex(id_host.sign_pk, pk_hex);
    strcat(test_uri, pk_hex);

    assert(wex_p2p_join_invite(&client, test_uri) == 0);

    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&host, 5);
        wex_p2p_poll(&client, 5);
        usleep(3000);
    }

    /* Client and host should establish session via LAN endpoint despite WAN endpoint presence */
    assert(client.peer_count >= 1);
    assert(memcmp(client.peers[0].pk, id_host.sign_pk, 32) == 0);

    /* Test direct URI format wex://<node_id>?wan=<ip>:<port>&lan=<ip>:<port> */
    wex_identity_t id_direct;
    assert(wex_identity_init(&id_direct, NULL, "DirectNode") == 0);
    wex_p2p_node_t direct_node;
    assert(wex_p2p_init(&direct_node, &id_direct, 31203) == 0);

    char direct_uri[256];
    char direct_pk_hex[65];
    wex_id_to_hex(id_direct.sign_pk, direct_pk_hex);
    snprintf(direct_uri, sizeof(direct_uri), "wex://%s?wan=203.0.113.1:54321&lan=127.0.0.1:31203", direct_pk_hex);

    assert(wex_p2p_join_invite(&client, direct_uri) == 0);

    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&direct_node, 5);
        wex_p2p_poll(&client, 5);
        usleep(3000);
    }

    bool direct_found = false;
    for (size_t i = 0; i < client.peer_count; i++) {
        if (memcmp(client.peers[i].pk, id_direct.sign_pk, 32) == 0) {
            direct_found = true;
            break;
        }
    }
    assert(direct_found == true);

    /* Verify continuous UDP hole-punch spraying tracker */
    struct sockaddr_in target_ep;
    memset(&target_ep, 0, sizeof(target_ep));
    target_ep.sin_family = AF_INET;
    target_ep.sin_port = htons(31201);
    inet_pton(AF_INET, "127.0.0.1", &target_ep.sin_addr);

    wex_p2p_punch_hole(&client, &target_ep, id_host.sign_pk);
    assert(client.punch_tracker_count >= 1);
    size_t trk_idx = client.punch_tracker_count - 1;
    client.punch_trackers[trk_idx].target_addr = target_ep;
    client.punch_trackers[trk_idx].has_target_addr = true;
    client.punch_trackers[trk_idx].direct_success = false;
    client.punch_trackers[trk_idx].relay_fallback = false;

    uint64_t t0 = client.punch_trackers[trk_idx].punch_started_ms;
    wex_p2p_maintain(&client, t0 + 200);
    assert(client.punch_trackers[trk_idx].last_spray_ms == t0 + 200);
    wex_p2p_maintain(&client, t0 + 400);
    assert(client.punch_trackers[trk_idx].last_spray_ms == t0 + 400);

    TEST_PASS("Multi-endpoint URI (wan + lan) generation, dual connection, and continuous UDP spraying");

    wex_p2p_close(&host);
    wex_p2p_close(&client);
    wex_p2p_close(&direct_node);
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex NAT Traversal & STUN Tests            \n");
    printf("====================================================\n");

    test_addr_formatting();
    test_local_mock_stun();
    test_live_stun_if_available();
    test_coordinated_nat_hole_punch();
    test_symmetric_nat_relay_fallback();
    test_multi_endpoint_invite_and_spraying();

    printf("====================================================\n");
    printf("  ALL NAT & STUN TESTS PASSED SUCCESSFULLY!         \n");
    printf("====================================================\n");
    return 0;
}
