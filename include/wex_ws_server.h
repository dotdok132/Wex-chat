#ifndef WEX_WS_SERVER_H
#define WEX_WS_SERVER_H

#include "wex_models.h"
#include <stdint.h>
#include <stdbool.h>

#define WEX_DEFAULT_WS_PORT 9000
#define WEX_MAX_CLIENTS 16
#define WEX_MAX_HISTORY 256

typedef struct {
    int server_fd;
    int port;
    bool running;
    wex_identity_t identity;
    size_t space_count;
    wex_space_t spaces[WEX_MAX_SPACES];
    size_t history_count;
    wex_chat_msg_t history[WEX_MAX_HISTORY];
    int client_fds[WEX_MAX_CLIENTS];
} wex_server_t;

/* Initialize server state and default space */
int wex_server_init(wex_server_t *srv, int port, const char *nickname);

/* Start listening on 127.0.0.1:port */
int wex_server_start(wex_server_t *srv);

/* Process pending network events (one tick of the event loop) */
int wex_server_poll(wex_server_t *srv, int timeout_ms);

/* Broadcast a text frame to all connected WebSocket clients */
void wex_server_broadcast(wex_server_t *srv, const char *text);

/* Gracefully shut down server */
void wex_server_stop(wex_server_t *srv);

#endif /* WEX_WS_SERVER_H */
