#define _GNU_SOURCE
#include "wex_ws_server.h"
#include "wex_sha1.h"
#include "../../vendor/cjson/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static const char *FALLBACK_HTML = 
"<!DOCTYPE html><html><head><meta charset='utf-8'><title>Wex</title></head>"
"<body style='background:#1e1f22;color:#fff;font-family:sans-serif;padding:20px;'>"
"<h2>Wex-Chat Daemon</h2><p>Please place web/index.html in working directory.</p></body></html>";

static void serve_html_file(int fd) {
    char *buffer = NULL;
    size_t length = 0;
    FILE *f = fopen("web/index.html", "rb");
    if (!f) {
        f = fopen("../web/index.html", "rb");
    }

    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            length = (size_t)sz;
            buffer = malloc(length + 1);
            if (buffer) {
                size_t rd = fread(buffer, 1, length, f);
                buffer[rd] = '\0';
            }
        }
        fclose(f);
    }

    const char *content = buffer ? buffer : FALLBACK_HTML;
    size_t content_len = buffer ? length : strlen(FALLBACK_HTML);

    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html; charset=UTF-8\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n\r\n", content_len);

    ssize_t h = write(fd, header, strlen(header));
    (void)h;
    ssize_t b = write(fd, content, content_len);
    (void)b;

    if (buffer) free(buffer);
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

int wex_server_init(wex_server_t *srv, int port, const char *nickname) {
    if (!srv) return -1;
    memset(srv, 0, sizeof(*srv));
    srv->port = (port > 0) ? port : WEX_DEFAULT_WS_PORT;
    srv->server_fd = -1;
    srv->running = false;

    for (int i = 0; i < WEX_MAX_CLIENTS; i++) {
        srv->client_fds[i] = -1;
    }

    if (wex_identity_init(&srv->identity, NULL, nickname) != 0) {
        return -1;
    }

    /* Initialize default space */
    wex_space_create(&srv->spaces[0], "Wex Realm");
    srv->space_count = 1;

    return 0;
}

int wex_server_start(wex_server_t *srv) {
    if (!srv) return -1;

    srv->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->server_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(srv->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 */
    addr.sin_port = htons((uint16_t)srv->port);

    if (bind(srv->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(srv->server_fd);
        srv->server_fd = -1;
        return -1;
    }

    if (listen(srv->server_fd, 16) < 0) {
        perror("listen");
        close(srv->server_fd);
        srv->server_fd = -1;
        return -1;
    }

    set_nonblocking(srv->server_fd);
    srv->running = true;

    printf("[+] Wex WebSocket daemon listening on http://127.0.0.1:%d\n", srv->port);
    return 0;
}

static void send_ws_frame(int fd, const char *msg, size_t len) {
    if (fd < 0 || !msg) return;

    uint8_t header[10];
    size_t header_len = 0;

    header[0] = 0x81; /* FIN=1, Text opcode */
    if (len < 126) {
        header[1] = (uint8_t)len;
        header_len = 2;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (uint8_t)((len >> 8) & 0xFF);
        header[3] = (uint8_t)(len & 0xFF);
        header_len = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (uint8_t)((len >> ((7 - i) * 8)) & 0xFF);
        }
        header_len = 10;
    }

    ssize_t w1 = write(fd, header, header_len);
    (void)w1;
    ssize_t w2 = write(fd, msg, len);
    (void)w2;
}

void wex_server_broadcast(wex_server_t *srv, const char *text) {
    if (!srv || !text) return;
    size_t len = strlen(text);
    for (int i = 0; i < WEX_MAX_CLIENTS; i++) {
        if (srv->client_fds[i] >= 0) {
            send_ws_frame(srv->client_fds[i], text, len);
        }
    }
}

static void send_status_json(wex_server_t *srv, int client_fd) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "status");

    cJSON *ident = cJSON_CreateObject();
    cJSON_AddStringToObject(ident, "nickname", srv->identity.nickname);
    cJSON_AddStringToObject(ident, "status", srv->identity.status);
    char pk_hex[65];
    wex_id_to_hex(srv->identity.sign_pk, pk_hex);
    cJSON_AddStringToObject(ident, "public_key", pk_hex);
    cJSON_AddItemToObject(root, "identity", ident);

    cJSON *spaces = cJSON_CreateArray();
    for (size_t s = 0; s < srv->space_count; s++) {
        cJSON *sp = cJSON_CreateObject();
        char sp_hex[65];
        wex_id_to_hex(srv->spaces[s].space_id, sp_hex);
        cJSON_AddStringToObject(sp, "id", sp_hex);
        cJSON_AddStringToObject(sp, "name", srv->spaces[s].name);

        cJSON *chs = cJSON_CreateArray();
        for (size_t c = 0; c < srv->spaces[s].channel_count; c++) {
            cJSON_AddItemToArray(chs, cJSON_CreateString(srv->spaces[s].channels[c].name));
        }
        cJSON_AddItemToObject(sp, "channels", chs);
        cJSON_AddItemToArray(spaces, sp);
    }
    cJSON_AddItemToObject(root, "spaces", spaces);

    cJSON *hist = cJSON_CreateArray();
    for (size_t h = 0; h < srv->history_count; h++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "type", "message");
        cJSON_AddStringToObject(m, "author", srv->history[h].author);
        cJSON_AddStringToObject(m, "channel", srv->history[h].channel);
        cJSON_AddStringToObject(m, "content", srv->history[h].content);
        cJSON_AddNumberToObject(m, "timestamp_ms", (double)srv->history[h].timestamp_ms);
        cJSON_AddItemToArray(hist, m);
    }
    cJSON_AddItemToObject(root, "history", hist);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        send_ws_frame(client_fd, json_str, strlen(json_str));
        free(json_str);
    }
    cJSON_Delete(root);
}

static void handle_client_json(wex_server_t *srv, int client_fd, const char *payload) {
    cJSON *root = cJSON_Parse(payload);
    if (!root) return;

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(action->valuestring, "get_status") == 0) {
        send_status_json(srv, client_fd);
    } else if (strcmp(action->valuestring, "set_profile") == 0) {
        cJSON *nick = cJSON_GetObjectItem(root, "nickname");
        if (nick && cJSON_IsString(nick)) {
            strncpy(srv->identity.nickname, nick->valuestring, sizeof(srv->identity.nickname) - 1);
            send_status_json(srv, client_fd);
        }
    } else if (strcmp(action->valuestring, "create_space") == 0) {
        cJSON *name = cJSON_GetObjectItem(root, "name");
        if (name && cJSON_IsString(name) && srv->space_count < WEX_MAX_SPACES) {
            wex_space_create(&srv->spaces[srv->space_count], name->valuestring);
            srv->space_count++;
            send_status_json(srv, client_fd);
        }
    } else if (strcmp(action->valuestring, "send_message") == 0) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        cJSON *channel = cJSON_GetObjectItem(root, "channel");
        if (text && cJSON_IsString(text)) {
            wex_chat_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            if (srv->space_count > 0) {
                memcpy(msg.space_id, srv->spaces[0].space_id, WEX_ID_LEN);
            }
            if (channel && cJSON_IsString(channel)) {
                strncpy(msg.channel, channel->valuestring, sizeof(msg.channel) - 1);
            } else {
                strncpy(msg.channel, "general", sizeof(msg.channel) - 1);
            }
            strncpy(msg.content, text->valuestring, sizeof(msg.content) - 1);

            /* Sign message with Ed25519 identity */
            wex_chat_msg_sign(&msg, &srv->identity);

            /* Store in local history */
            if (srv->history_count < WEX_MAX_HISTORY) {
                srv->history[srv->history_count++] = msg;
            }

            /* Broadcast message event to all connected UI clients */
            cJSON *out = cJSON_CreateObject();
            cJSON_AddStringToObject(out, "type", "message");
            cJSON_AddStringToObject(out, "author", msg.author);
            cJSON_AddStringToObject(out, "channel", msg.channel);
            cJSON_AddStringToObject(out, "content", msg.content);
            cJSON_AddNumberToObject(out, "timestamp_ms", (double)msg.timestamp_ms);
            char sig_hex[129];
            for (int i = 0; i < 64; i++) {
                sprintf(sig_hex + (i * 2), "%02x", msg.signature[i]);
            }
            sig_hex[128] = '\0';
            cJSON_AddStringToObject(out, "signature", sig_hex);

            char *json_out = cJSON_PrintUnformatted(out);
            if (json_out) {
                wex_server_broadcast(srv, json_out);
                free(json_out);
            }
            cJSON_Delete(out);
        }
    }

    cJSON_Delete(root);
}

static bool perform_handshake(int fd, const char *req, size_t req_len) {
    (void)req_len;
    const char *key_hdr = "Sec-WebSocket-Key: ";
    const char *key_start = strstr(req, key_hdr);

    if (!key_start) {
        /* Not a WebSocket handshake request - serve standard Discord HTML page! */
        if (strstr(req, "GET ") == req) {
            serve_html_file(fd);
        }
        return false;
    }

    key_start += strlen(key_hdr);
    const char *key_end = strstr(key_start, "\r\n");
    if (!key_end) return false;

    char client_key[128];
    size_t key_len = (size_t)(key_end - key_start);
    if (key_len >= sizeof(client_key)) return false;
    memcpy(client_key, key_start, key_len);
    client_key[key_len] = '\0';

    /* Concatenate key + WS_GUID */
    char concat[256];
    snprintf(concat, sizeof(concat), "%s%s", client_key, WS_GUID);

    /* SHA-1 hash */
    wex_sha1_ctx sha_ctx;
    uint8_t digest[20];
    wex_sha1_init(&sha_ctx);
    wex_sha1_update(&sha_ctx, (uint8_t *)concat, strlen(concat));
    wex_sha1_final(&sha_ctx, digest);

    /* Base64 encode accept key */
    char accept_key[64];
    wex_base64_encode(digest, 20, accept_key);

    /* Send HTTP 101 Switching Protocols */
    char response[512];
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n", accept_key);

    ssize_t w = write(fd, response, strlen(response));
    (void)w;
    return true;
}

int wex_server_poll(wex_server_t *srv, int timeout_ms) {
    if (!srv || srv->server_fd < 0) return -1;

    struct pollfd fds[WEX_MAX_CLIENTS + 1];
    int count = 0;

    fds[0].fd = srv->server_fd;
    fds[0].events = POLLIN;
    count = 1;

    int client_map[WEX_MAX_CLIENTS + 1];
    client_map[0] = -1;

    for (int i = 0; i < WEX_MAX_CLIENTS; i++) {
        if (srv->client_fds[i] >= 0) {
            fds[count].fd = srv->client_fds[i];
            fds[count].events = POLLIN;
            client_map[count] = i;
            count++;
        }
    }

    int ret = poll(fds, (nfds_t)count, timeout_ms);
    if (ret <= 0) return ret;

    /* Check new incoming connection */
    if (fds[0].revents & POLLIN) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int new_fd = accept(srv->server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (new_fd >= 0) {
            set_nonblocking(new_fd);

            /* Read initial request for handshake */
            char buf[4096];
            usleep(10000); /* Small sleep to let TCP packet arrive */
            ssize_t n = read(new_fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                if (perform_handshake(new_fd, buf, (size_t)n)) {
                    /* Slot client */
                    int slotted = 0;
                    for (int i = 0; i < WEX_MAX_CLIENTS; i++) {
                        if (srv->client_fds[i] < 0) {
                            srv->client_fds[i] = new_fd;
                            slotted = 1;
                            break;
                        }
                    }
                    if (!slotted) close(new_fd);
                } else {
                    close(new_fd);
                }
            } else {
                close(new_fd);
            }
        }
    }

    /* Check data from existing clients */
    for (int idx = 1; idx < count; idx++) {
        if (fds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
            int slot = client_map[idx];
            int cfd = srv->client_fds[slot];

            uint8_t frame_buf[4096];
            ssize_t r = read(cfd, frame_buf, sizeof(frame_buf));
            if (r <= 0) {
                close(cfd);
                srv->client_fds[slot] = -1;
                continue;
            }

            /* Basic WS frame decode */
            if (r < 2) continue;
            uint8_t opcode = frame_buf[0] & 0x0F;
            if (opcode == 0x08) { /* Connection close */
                close(cfd);
                srv->client_fds[slot] = -1;
                continue;
            }

            bool masked = (frame_buf[1] & 0x80) != 0;
            uint64_t payload_len = frame_buf[1] & 0x7F;
            size_t header_len = 2;

            if (payload_len == 126) {
                if (r < 4) continue;
                payload_len = ((uint64_t)frame_buf[2] << 8) | frame_buf[3];
                header_len = 4;
            }

            uint8_t mask_key[4] = {0};
            if (masked) {
                if ((size_t)r < header_len + 4) continue;
                memcpy(mask_key, frame_buf + header_len, 4);
                header_len += 4;
            }

            if ((size_t)r >= header_len + payload_len && payload_len < sizeof(frame_buf)) {
                uint8_t *payload = frame_buf + header_len;
                if (masked) {
                    for (size_t i = 0; i < payload_len; i++) {
                        payload[i] ^= mask_key[i % 4];
                    }
                }
                payload[payload_len] = '\0';
                handle_client_json(srv, cfd, (const char *)payload);
            }
        }
    }

    return 1;
}

void wex_server_stop(wex_server_t *srv) {
    if (!srv) return;
    for (int i = 0; i < WEX_MAX_CLIENTS; i++) {
        if (srv->client_fds[i] >= 0) {
            close(srv->client_fds[i]);
            srv->client_fds[i] = -1;
        }
    }
    if (srv->server_fd >= 0) {
        close(srv->server_fd);
        srv->server_fd = -1;
    }
    srv->running = false;
}
