#ifndef WEX_WS_SERVER_H
#define WEX_WS_SERVER_H

#include "wex_models.h"
#include "wex_p2p.h"
#include <stdint.h>
#include <stdbool.h>

#define WEX_DEFAULT_WS_PORT  9000
#define WEX_DEFAULT_UDP_PORT 9001
#define WEX_MAX_CLIENTS 16
#define WEX_MAX_HISTORY 256

typedef struct {
    int ws_server_fd;
    int ws_port;
    int udp_port;
    bool running;
    wex_identity_t identity;
    size_t space_count;
    wex_space_t spaces[WEX_MAX_SPACES];
    size_t history_count;
    wex_chat_msg_t history[WEX_MAX_HISTORY];
    int client_fds[WEX_MAX_CLIENTS];

    wex_journal_t *journal;
    wex_p2p_node_t p2p;
} wex_server_t;

/* Initialize server state, identity, and P2P engine */
int wex_server_init(wex_server_t *srv, int ws_port, int udp_port, const char *nickname);

/* Start listening on WS and bind UDP */
int wex_server_start(wex_server_t *srv);

/* Connect to remote P2P peer */
int wex_server_connect_peer(wex_server_t *srv, const char *ip_str, int port);

/* Process pending network events for both WS and P2P UDP */
int wex_server_poll(wex_server_t *srv, int timeout_ms);

/* Broadcast a text frame to all connected WebSocket clients */
void wex_server_broadcast(wex_server_t *srv, const char *text);

/* Gracefully shut down server */
void wex_server_stop(wex_server_t *srv);

#endif /* WEX_WS_SERVER_H */
