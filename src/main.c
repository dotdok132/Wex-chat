#include "wex_ws_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static volatile int keep_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    keep_running = 0;
}

int main(int argc, char **argv) {
    int port = WEX_DEFAULT_WS_PORT;
    const char *nickname = "dotdok";

    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0) port = WEX_DEFAULT_WS_PORT;
    }
    if (argc >= 3) {
        nickname = argv[2];
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("====================================================\n");
    printf("  Wex-Chat Daemon v0.1.0-alpha (Sovereign Discord)\n");
    printf("====================================================\n");
    printf("[*] User nickname: %s\n", nickname);

    wex_server_t server;
    if (wex_server_init(&server, port, nickname) != 0) {
        fprintf(stderr, "[-] Failed to initialize Wex server\n");
        return 1;
    }

    char pk_hex[65];
    wex_id_to_hex(server.identity.sign_pk, pk_hex);
    printf("[*] Ed25519 Public ID: %s\n", pk_hex);

    if (wex_server_start(&server) != 0) {
        fprintf(stderr, "[-] Failed to start WebSocket daemon on port %d\n", port);
        return 1;
    }

    printf("[+] Open your browser at: http://127.0.0.1:%d\n", port);
    printf("[*] Daemon running. Press Ctrl+C to stop.\n");

    while (keep_running) {
        wex_server_poll(&server, 100);
    }

    printf("\n[*] Stopping Wex server...\n");
    wex_server_stop(&server);
    printf("[+] Daemon terminated gracefully.\n");
    return 0;
}
