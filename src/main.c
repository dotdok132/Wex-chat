#include "wex_ws_server.h"
#include "wex_nat.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static volatile int keep_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    keep_running = 0;
}

int main(int argc, char **argv) {
    int ws_port = WEX_DEFAULT_WS_PORT;
    int udp_port = WEX_DEFAULT_UDP_PORT;
    const char *nickname = "dotdok";
    const char *peer_target = NULL;

    if (argc >= 2) {
        ws_port = atoi(argv[1]);
        if (ws_port <= 0) ws_port = WEX_DEFAULT_WS_PORT;
    }
    if (argc >= 3) {
        udp_port = atoi(argv[2]);
        if (udp_port <= 0) udp_port = ws_port + 1;
    } else {
        udp_port = ws_port + 1;
    }
    if (argc >= 4) {
        nickname = argv[3];
    }
    if (argc >= 5) {
        peer_target = argv[4];
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("====================================================\n");
    printf("  Wex-Chat Daemon v0.1.0-alpha (Sovereign Discord)\n");
    printf("====================================================\n");
    printf("[*] User nickname: %s\n", nickname);

    wex_server_t server;
    if (wex_server_init(&server, ws_port, udp_port, nickname) != 0) {
        fprintf(stderr, "[-] Failed to initialize Wex server\n");
        return 1;
    }

    char pk_hex[65];
    wex_id_to_hex(server.identity.sign_pk, pk_hex);
    printf("[*] Ed25519 Public ID: %s\n", pk_hex);

    if (wex_server_start(&server) != 0) {
        fprintf(stderr, "[-] Failed to start daemon\n");
        return 1;
    }

    /* WAN & NAT External Discovery via STUN RFC 5389 */
    printf("[*] Probing external WAN endpoint via STUN...\n");
    struct sockaddr_in ext_addr;
    if (wex_nat_auto_discover(server.p2p.udp_fd, &ext_addr) == 0) {
        char ext_str[64];
        wex_addr_to_string(&ext_addr, ext_str, sizeof(ext_str));
        server.p2p.external_addr = ext_addr;
        server.p2p.has_external_addr = true;
        printf("[+] External WAN Endpoint: %s (Publicly reachable over Internet)\n", ext_str);
    } else {
        printf("[*] External WAN: Operating behind local firewall / fallback to DHT relay\n");
    }

    /* If peer argument provided (e.g. 1.2.3.4:9001 or node.domain.com:9001), connect immediately */
    if (peer_target) {
        char ip_buf[128];
        strncpy(ip_buf, peer_target, sizeof(ip_buf) - 1);
        ip_buf[sizeof(ip_buf) - 1] = '\0';
        char *colon = strchr(ip_buf, ':');
        if (colon) {
            *colon = '\0';
            int peer_p = atoi(colon + 1);
            printf("[*] Auto-connecting to initial P2P peer %s:%d...\n", ip_buf, peer_p);
            wex_server_connect_peer(&server, ip_buf, peer_p);
        }
    }

    printf("[+] Open your browser at: http://127.0.0.1:%d\n", ws_port);
    printf("[*] Daemon running. Press Ctrl+C to stop.\n");

    while (keep_running) {
        wex_server_poll(&server, 100);
    }

    printf("\n[*] Stopping Wex server...\n");
    wex_server_stop(&server);
    printf("[+] Daemon terminated gracefully.\n");
    return 0;
}
