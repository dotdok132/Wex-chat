#define _GNU_SOURCE
#include "wex_ws_server.h"
#include "wex_nat.h"
#include "wex_persist.h"
#include "wex_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static volatile int keep_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    keep_running = 0;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --config <path>         Load configuration file (e.g. wex.conf)\n");
    printf("  --seed                  Run as headless P2P seed & relay node (no WebSocket/UI)\n");
    printf("  --port <port>           P2P UDP port (default: 9001)\n");
    printf("  --ws <port>             WebSocket & UI port (default: 9000)\n");
    printf("  --nick <name>           User nickname (default: dotdok)\n");
    printf("  --data-dir <path>       Directory for persistent storage (default: ./.wex_data)\n");
    printf("  --bootstrap <host:port> Initial bootstrap peer to connect to\n");
    printf("  -h, --help              Show this help message\n");
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
            break;
        }
    }

    wex_config_t conf;
    wex_config_set_defaults(&conf);

    if (config_path) {
        if (wex_config_load_file(&conf, config_path) == 0) {
            printf("[+] Loaded configuration from %s\n", config_path);
        } else {
            fprintf(stderr, "[-] Warning: Failed to load configuration from %s\n", config_path);
        }
    } else if (access("wex.conf", R_OK) == 0) {
        if (wex_config_load_file(&conf, "wex.conf") == 0) {
            printf("[+] Auto-loaded configuration from ./wex.conf\n");
        }
    }

    bool is_seed = conf.is_seed;
    int ws_port = conf.ws_port;
    int udp_port = conf.udp_port;
    const char *nickname = conf.nickname;
    const char *data_dir = conf.data_dir;
    const char *peer_target = NULL;

    /* Parse command-line arguments to allow overriding config */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            i++; /* already handled */
        } else if (strcmp(argv[i], "--seed") == 0) {
            is_seed = true;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            udp_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ws") == 0 && i + 1 < argc) {
            ws_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--nick") == 0 && i + 1 < argc) {
            nickname = argv[++i];
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (strcmp(argv[i], "--bootstrap") == 0 && i + 1 < argc) {
            peer_target = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            /* Positional arguments fallback: ws_port [udp_port] [nickname] [bootstrap] */
            static int pos = 0;
            if (pos == 0) ws_port = atoi(argv[i]);
            else if (pos == 1) udp_port = atoi(argv[i]);
            else if (pos == 2) nickname = argv[i];
            else if (pos == 3) peer_target = argv[i];
            pos++;
        }
    }

    if (is_seed && udp_port == WEX_DEFAULT_UDP_PORT && ws_port != WEX_DEFAULT_WS_PORT) {
        udp_port = ws_port;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("====================================================\n");
    if (is_seed) {
        printf("  Wex Seed & Relay Daemon (Headless Bootstrap Node)\n");
    } else {
        printf("  Wex-Chat Daemon v0.1.0-alpha (Sovereign Discord)\n");
    }
    printf("====================================================\n");

    /* Ensure data directory exists */
    wex_mkdir_p(data_dir);

    char key_path[560], journal_path[560];
    snprintf(key_path, sizeof(key_path), "%s/identity.key", data_dir);
    snprintf(journal_path, sizeof(journal_path), "%s/messages.log", data_dir);

    /* Persistent Identity Load or Generation */
    wex_identity_t ident;
    if (wex_identity_load_or_create(&ident, key_path, nickname) != 0) {
        fprintf(stderr, "[-] Failed to load or initialize persistent identity\n");
        return 1;
    }

    char pk_hex[65];
    wex_id_to_hex(ident.sign_pk, pk_hex);
    printf("[*] User nickname: %s\n", ident.nickname);
    printf("[*] Ed25519 Public ID: %s\n", pk_hex);
    printf("[+] Identity file: %s\n", key_path);

    /* Open persistent message journal */
    wex_journal_t *journal = NULL;
    if (wex_journal_open(&journal, journal_path) == 0) {
        printf("[+] Message journal: %s (%zu records on disk)\n", journal_path, wex_journal_count(journal));
    }

    if (is_seed) {
        /* Headless Seed / Relay Node Mode */
        wex_p2p_node_t p2p_node;
        if (wex_p2p_init(&p2p_node, &ident, udp_port) != 0) {
            fprintf(stderr, "[-] Failed to initialize seed P2P engine\n");
            if (journal) wex_journal_close(journal);
            return 1;
        }

        if (journal) {
            wex_p2p_set_journal(&p2p_node, journal);
        }

        /* WAN STUN Discovery */
        printf("[*] Probing external WAN endpoint via STUN...\n");
        struct sockaddr_in ext_addr;
        if (wex_nat_auto_discover(p2p_node.udp_fd, &ext_addr) == 0) {
            char ext_str[64];
            wex_addr_to_string(&ext_addr, ext_str, sizeof(ext_str));
            p2p_node.external_addr = ext_addr;
            p2p_node.has_external_addr = true;
            printf("[+] Public WAN Seed Endpoint: %s\n", ext_str);
        }

        if (peer_target) {
            char ip_buf[128];
            strncpy(ip_buf, peer_target, sizeof(ip_buf) - 1);
            ip_buf[sizeof(ip_buf) - 1] = '\0';
            char *colon = strchr(ip_buf, ':');
            if (colon) {
                *colon = '\0';
                int peer_p = atoi(colon + 1);
                printf("[*] Auto-connecting to bootstrap peer %s:%d...\n", ip_buf, peer_p);
                wex_p2p_connect(&p2p_node, ip_buf, peer_p);
            }
        }

        for (size_t b = 0; b < conf.bootstrap_count; b++) {
            printf("[*] Auto-connecting to config bootstrap peer %s:%d...\n",
                   conf.bootstrap_peers[b].host, conf.bootstrap_peers[b].port);
            wex_p2p_connect(&p2p_node, conf.bootstrap_peers[b].host, conf.bootstrap_peers[b].port);
        }

        printf("[*] Seed node running on UDP :%d. Press Ctrl+C to stop.\n", udp_port);

        uint64_t last_stat_log = (uint64_t)time(NULL);
        while (keep_running) {
            wex_p2p_poll(&p2p_node, 100);

            uint64_t now_sec = (uint64_t)time(NULL);
            if (now_sec - last_stat_log >= 30) {
                last_stat_log = now_sec;
                printf("[*] [Heartbeat] Connected Peers: %zu | DHT Table Nodes: %zu\n",
                       p2p_node.peer_count, p2p_node.dht.total_contacts);
            }
        }

        printf("\n[*] Stopping Wex seed node...\n");
        wex_p2p_close(&p2p_node);
        if (journal) wex_journal_close(journal);
        printf("[+] Seed node terminated gracefully.\n");
        return 0;
    }

    /* Client / Full Server Mode */
    wex_server_t server;
    if (wex_server_init(&server, ws_port, udp_port, ident.nickname) != 0) {
        fprintf(stderr, "[-] Failed to initialize Wex server\n");
        if (journal) wex_journal_close(journal);
        return 1;
    }

    /* Apply persistent identity */
    server.identity = ident;
    server.p2p.identity = &server.identity;

    if (journal) {
        server.journal = journal;
        wex_p2p_set_journal(&server.p2p, journal);
    }

    if (wex_server_start(&server) != 0) {
        fprintf(stderr, "[-] Failed to start daemon\n");
        if (journal) wex_journal_close(journal);
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

    for (size_t b = 0; b < conf.bootstrap_count; b++) {
        printf("[*] Auto-connecting to config bootstrap peer %s:%d...\n",
               conf.bootstrap_peers[b].host, conf.bootstrap_peers[b].port);
        wex_server_connect_peer(&server, conf.bootstrap_peers[b].host, conf.bootstrap_peers[b].port);
    }

    printf("[+] Open your browser at: http://127.0.0.1:%d\n", ws_port);
    printf("[*] Daemon running. Press Ctrl+C to stop.\n");

    while (keep_running) {
        wex_server_poll(&server, 100);
    }

    printf("\n[*] Stopping Wex server...\n");
    wex_server_stop(&server);
    if (journal) wex_journal_close(journal);
    printf("[+] Daemon terminated gracefully.\n");
    return 0;
}
