#define _GNU_SOURCE
#include "wex_ws_server.h"
#include "wex_sha1.h"
#include "wex_crypto.h"
#include "wex_nat.h"
#include "wex_persist.h"
#include "wex_permissions.h"
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
#include <time.h>

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

static void serve_static_file(int fd, const char *path, const char *content_type) {
    char *buffer = NULL;
    size_t length = 0;
    FILE *f = fopen(path, "rb");
    if (!f && strncmp(path, "../", 3) != 0) {
        char alt_path[256];
        snprintf(alt_path, sizeof(alt_path), "../%s", path);
        f = fopen(alt_path, "rb");
    }

    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            length = (size_t)sz;
            buffer = (char *)malloc(length + 1);
            if (buffer) {
                size_t rd = fread(buffer, 1, length, f);
                buffer[rd] = '\0';
            }
        }
        fclose(f);
    }

    if (!buffer) {
        const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ssize_t w = write(fd, nf, strlen(nf));
        (void)w;
        return;
    }

    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n\r\n", content_type, length);

    ssize_t h = write(fd, header, strlen(header));
    (void)h;
    ssize_t b = write(fd, buffer, length);
    (void)b;

    free(buffer);
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
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

static void on_p2p_incoming_msg(const wex_chat_msg_t *msg, void *user_data) {
    wex_server_t *srv = (wex_server_t *)user_data;
    if (!srv || !msg) return;

    /* Check duplicate */
    for (size_t i = 0; i < srv->history_count; i++) {
        if (memcmp(srv->history[i].signature, msg->signature, 64) == 0) {
            return; /* Already have this message */
        }
    }

    bool is_dm = (strncmp(msg->channel, "dm:", 3) == 0);
    bool is_encrypted = ((msg->enc_flags & WEX_ENC_DM_E2EE) != 0) || is_dm;

    char author_pk_hex[65];
    wex_id_to_hex(msg->author_pk, author_pk_hex);

    /* Save to history */
    if (srv->history_count < WEX_MAX_HISTORY) {
        wex_chat_msg_t saved = *msg;
        if (is_encrypted) {
            saved.enc_flags |= WEX_ENC_DM_E2EE;
        }
        srv->history[srv->history_count++] = saved;
    }

    /* Broadcast to Web UI */
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "type", "message");
    cJSON_AddStringToObject(out, "author", msg->author);
    cJSON_AddStringToObject(out, "author_pk", author_pk_hex);
    if (is_dm) {
        cJSON_AddStringToObject(out, "peer_pk", author_pk_hex);
    }
    cJSON_AddStringToObject(out, "channel", msg->channel);
    cJSON_AddStringToObject(out, "content", msg->content);
    cJSON_AddBoolToObject(out, "verified", true);
    cJSON_AddBoolToObject(out, "encrypted", is_encrypted);
    cJSON_AddNumberToObject(out, "timestamp_ms", (double)msg->timestamp_ms);

    char sig_hex[129];
    for (int i = 0; i < 64; i++) sprintf(sig_hex + (i * 2), "%02x", msg->signature[i]);
    sig_hex[128] = '\0';
    cJSON_AddStringToObject(out, "signature", sig_hex);
    cJSON_AddStringToObject(out, "reactions", "");

    char *json_out = cJSON_PrintUnformatted(out);
    if (json_out) {
        wex_server_broadcast(srv, json_out);
        free(json_out);
    }
    cJSON_Delete(out);
}

static void on_p2p_peer_event(const wex_peer_t *peer, bool joined, void *user_data) {
    wex_server_t *srv = (wex_server_t *)user_data;
    if (!srv || !peer) return;

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "type", joined ? "peer_joined" : "peer_left");
    cJSON_AddStringToObject(out, "nickname", peer->nickname);
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer->addr.sin_addr, ip_str, sizeof(ip_str));
    cJSON_AddStringToObject(out, "ip", ip_str);
    cJSON_AddNumberToObject(out, "port", ntohs(peer->addr.sin_port));

    char *json_out = cJSON_PrintUnformatted(out);
    if (json_out) {
        wex_server_broadcast(srv, json_out);
        free(json_out);
    }
    cJSON_Delete(out);
}

static size_t on_p2p_sync_request(const uint8_t space_id[32], const char *channel, wex_chat_msg_t out_msgs[], size_t max_msgs, void *user_data) {
    wex_server_t *srv = (wex_server_t *)user_data;
    if (!srv || !channel) return 0;

    size_t count = 0;
    for (size_t i = 0; i < srv->history_count && count < max_msgs; i++) {
        if (memcmp(srv->history[i].space_id, space_id, 32) == 0 &&
            strcmp(srv->history[i].channel, channel) == 0) {
            out_msgs[count++] = srv->history[i];
        }
    }
    return count;
}

static void on_p2p_blob_event(const wex_blob_info_t *info, const uint8_t *data, void *user_data) {
    wex_server_t *srv = (wex_server_t *)user_data;
    if (!srv || !info) return;

    char hash_hex[65];
    wex_id_to_hex(info->root_hash, hash_hex);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "type", (data != NULL) ? "blob_received" : "blob_offered");
    cJSON_AddStringToObject(out, "hash", hash_hex);
    cJSON_AddStringToObject(out, "filename", info->filename);
    cJSON_AddStringToObject(out, "mime_type", info->mime_type);
    cJSON_AddNumberToObject(out, "size", (double)info->total_size);

    char *json_out = cJSON_PrintUnformatted(out);
    if (json_out) {
        wex_server_broadcast(srv, json_out);
        free(json_out);
    }
    cJSON_Delete(out);
}

static void on_p2p_action_event(const wex_msg_action_t *action, void *user_data) {
    wex_server_t *srv = (wex_server_t *)user_data;
    if (!srv || !action) return;

    wex_chat_msg_t *target_msg = NULL;
    size_t target_idx = 0;
    for (size_t h = 0; h < srv->history_count; h++) {
        if (memcmp(srv->history[h].signature, action->target_sig, 64) == 0) {
            target_msg = &srv->history[h];
            target_idx = h;
            break;
        }
    }
    if (!target_msg) return;

    if (wex_msg_action_apply(&srv->reactions, NULL, target_msg, action) == 0) {
        char sig_hex[129];
        for (int i = 0; i < 64; i++) sprintf(sig_hex + (i * 2), "%02x", action->target_sig[i]);
        sig_hex[128] = '\0';

        cJSON *out = cJSON_CreateObject();
        if (action->action_type == WEX_ACTION_REACT) {
            char summary[128];
            wex_reactions_get_summary(&srv->reactions, action->target_sig, summary, sizeof(summary));
            cJSON_AddStringToObject(out, "type", "reaction_updated");
            cJSON_AddStringToObject(out, "target_sig", sig_hex);
            cJSON_AddStringToObject(out, "summary", summary);
        } else if (action->action_type == WEX_ACTION_EDIT) {
            cJSON_AddStringToObject(out, "type", "message_edited");
            cJSON_AddStringToObject(out, "target_sig", sig_hex);
            cJSON_AddStringToObject(out, "content", target_msg->content);
        } else if (action->action_type == WEX_ACTION_DELETE) {
            cJSON_AddStringToObject(out, "type", "message_deleted");
            cJSON_AddStringToObject(out, "target_sig", sig_hex);
            if (target_idx + 1 < srv->history_count) {
                memmove(&srv->history[target_idx], &srv->history[target_idx + 1],
                        (srv->history_count - target_idx - 1) * sizeof(wex_chat_msg_t));
            }
            srv->history_count--;
        }
        char *json_out = cJSON_PrintUnformatted(out);
        if (json_out) {
            wex_server_broadcast(srv, json_out);
            free(json_out);
        }
        cJSON_Delete(out);
    }
}

static void on_p2p_voice_event(const char *channel, const wex_voice_hdr_t *hdr, const uint8_t *audio_data, size_t audio_len, void *user_data) {
    wex_server_t *srv = (wex_server_t *)user_data;
    if (!srv || !hdr || !srv->voice_room.is_in_call) return;
    if (channel && strcmp(srv->voice_room.channel, channel) != 0) return;

    /* Push incoming frame to local jitter buffer */
    wex_jitter_buffer_push(&srv->voice_room.jitter, hdr, audio_data, audio_len);

    /* Broadcast voice stream chunk to connected Web UI clients */
    if (audio_data && audio_len > 0) {
        char *b64 = wex_b64_encode(audio_data, audio_len);
        if (b64) {
            cJSON *out = cJSON_CreateObject();
            cJSON_AddStringToObject(out, "type", "voice_stream");
            cJSON_AddStringToObject(out, "channel", channel ? channel : "");
            cJSON_AddNumberToObject(out, "ssrc", (double)hdr->ssrc);
            cJSON_AddNumberToObject(out, "seq", (double)hdr->sequence);
            cJSON_AddStringToObject(out, "data_base64", b64);
            char *json_out = cJSON_PrintUnformatted(out);
            if (json_out) {
                wex_server_broadcast(srv, json_out);
                free(json_out);
            }
            cJSON_Delete(out);
            free(b64);
        }
    }
}

int wex_server_init(wex_server_t *srv, int ws_port, int udp_port, const char *nickname) {
    if (!srv) return -1;
    memset(srv, 0, sizeof(*srv));
    srv->ws_port = (ws_port > 0) ? ws_port : WEX_DEFAULT_WS_PORT;
    srv->udp_port = (udp_port > 0) ? udp_port : WEX_DEFAULT_UDP_PORT;
    srv->ws_server_fd = -1;
    srv->running = false;

    for (int i = 0; i < WEX_MAX_CLIENTS; i++) {
        srv->client_fds[i] = -1;
    }

    if (wex_identity_init(&srv->identity, NULL, nickname) != 0) {
        return -1;
    }

    srv->space_count = 0;

    /* Initialize P2P UDP engine */
    if (wex_p2p_init(&srv->p2p, &srv->identity, srv->udp_port) != 0) {
        return -1;
    }
    wex_p2p_set_data_dir(&srv->p2p, "./.wex_data");
    srv->p2p.lan_discovery_enabled = true;
    wex_p2p_set_callbacks(&srv->p2p, on_p2p_incoming_msg, on_p2p_peer_event, srv);
    wex_p2p_set_sync_callback(&srv->p2p, on_p2p_sync_request);
    wex_p2p_set_blob_callback(&srv->p2p, on_p2p_blob_event);
    wex_p2p_set_action_callback(&srv->p2p, on_p2p_action_event);
    wex_p2p_set_voice_callback(&srv->p2p, on_p2p_voice_event);

    wex_reactions_store_init(&srv->reactions);
    wex_voice_room_init(&srv->voice_room, "voice-lounge", (uint32_t)srv->udp_port);

    return 0;
}

int wex_server_start(wex_server_t *srv) {
    if (!srv) return -1;

    srv->ws_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->ws_server_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(srv->ws_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)srv->ws_port);

    if (bind(srv->ws_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(WS)");
        close(srv->ws_server_fd);
        srv->ws_server_fd = -1;
        return -1;
    }

    if (listen(srv->ws_server_fd, 16) < 0) {
        perror("listen(WS)");
        close(srv->ws_server_fd);
        srv->ws_server_fd = -1;
        return -1;
    }

    set_nonblocking(srv->ws_server_fd);
    srv->running = true;

    printf("[+] Wex WebSocket daemon on http://127.0.0.1:%d\n", srv->ws_port);
    printf("[+] Wex P2P UDP engine on 0.0.0.0:%d\n", srv->udp_port);
    return 0;
}

int wex_server_connect_peer(wex_server_t *srv, const char *ip_str, int port) {
    if (!srv || !ip_str) return -1;
    return wex_p2p_connect(&srv->p2p, ip_str, port);
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
    char xpk_hex[65];
    wex_id_to_hex(srv->identity.x25519_pk, xpk_hex);
    cJSON_AddStringToObject(ident, "x25519_key", xpk_hex);
    cJSON_AddNumberToObject(ident, "udp_port", srv->udp_port);
    cJSON_AddNumberToObject(ident, "dht_contacts", (double)srv->p2p.dht.total_contacts);
    cJSON_AddNumberToObject(ident, "offline_shards", (double)srv->p2p.offline_store.count);
    if (srv->p2p.has_external_addr) {
        char ext_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &srv->p2p.external_addr.sin_addr, ext_ip, sizeof(ext_ip));
        char ext_ep[64];
        snprintf(ext_ep, sizeof(ext_ep), "%s:%d", ext_ip, (int)ntohs(srv->p2p.external_addr.sin_port));
        cJSON_AddStringToObject(ident, "external_endpoint", ext_ep);
    } else {
        cJSON_AddStringToObject(ident, "external_endpoint", "");
    }
    char invite_uri[512];
    if (wex_p2p_create_invite(&srv->p2p, invite_uri, sizeof(invite_uri)) == 0) {
        cJSON_AddStringToObject(ident, "invite_uri", invite_uri);
    }
    cJSON_AddItemToObject(root, "identity", ident);

    cJSON *spaces = cJSON_CreateArray();
    size_t sc = (srv->p2p.space_count > 0) ? srv->p2p.space_count : srv->space_count;
    wex_space_t *src_spaces = (srv->p2p.space_count > 0) ? srv->p2p.spaces : srv->spaces;

    for (size_t s = 0; s < sc; s++) {
        cJSON *sp = cJSON_CreateObject();
        char sp_hex[65];
        wex_id_to_hex(src_spaces[s].space_id, sp_hex);
        cJSON_AddStringToObject(sp, "id", sp_hex);
        cJSON_AddStringToObject(sp, "name", src_spaces[s].name);

        cJSON *chs = cJSON_CreateArray();
        for (size_t c = 0; c < src_spaces[s].channel_count; c++) {
            cJSON_AddItemToArray(chs, cJSON_CreateString(src_spaces[s].channels[c].name));
        }
        cJSON_AddItemToObject(sp, "channels", chs);
        cJSON_AddItemToArray(spaces, sp);
    }
    cJSON_AddItemToObject(root, "spaces", spaces);
    cJSON_AddStringToObject(root, "active_channel", srv->p2p.active_channel);

    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    uint64_t now_ms = (uint64_t)ts_now.tv_sec * 1000ULL + (uint64_t)ts_now.tv_nsec / 1000000ULL;

    cJSON *peers = cJSON_CreateArray();
    for (size_t p = 0; p < srv->p2p.peer_count; p++) {
        if (srv->p2p.peers[p].active) {
            cJSON *peer_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(peer_obj, "nickname", srv->p2p.peers[p].nickname);
            char pk_hex[65];
            wex_id_to_hex(srv->p2p.peers[p].pk, pk_hex);
            cJSON_AddStringToObject(peer_obj, "public_key", pk_hex);
            char ip_buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &srv->p2p.peers[p].addr.sin_addr, ip_buf, sizeof(ip_buf));
            cJSON_AddStringToObject(peer_obj, "ip", ip_buf);
            cJSON_AddNumberToObject(peer_obj, "port", ntohs(srv->p2p.peers[p].addr.sin_port));
            cJSON_AddBoolToObject(peer_obj, "use_relay", srv->p2p.peers[p].use_relay);
            bool is_online = (srv->p2p.peers[p].last_seen_ms > 0) &&
                             (now_ms >= srv->p2p.peers[p].last_seen_ms) &&
                             (now_ms - srv->p2p.peers[p].last_seen_ms < 45000ULL);
            cJSON_AddBoolToObject(peer_obj, "online", is_online);
            cJSON_AddItemToArray(peers, peer_obj);
        }
    }
    cJSON_AddItemToObject(root, "peers", peers);

    cJSON *dht_list = cJSON_CreateArray();
    size_t dht_added = 0;
    for (int b = 0; b < WEX_DHT_NUM_BUCKETS && dht_added < 24; b++) {
        for (size_t c = 0; c < srv->p2p.dht.buckets[b].count && dht_added < 24; c++) {
            const wex_dht_contact_t *ct = &srv->p2p.dht.buckets[b].contacts[c];
            cJSON *dnode = cJSON_CreateObject();
            char cid_hex[65];
            wex_id_to_hex(ct->id, cid_hex);
            cJSON_AddStringToObject(dnode, "id", cid_hex);
            char dip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &ct->addr.sin_addr, dip, sizeof(dip));
            cJSON_AddStringToObject(dnode, "ip", dip);
            cJSON_AddNumberToObject(dnode, "port", ntohs(ct->addr.sin_port));
            cJSON_AddNumberToObject(dnode, "bucket", b);
            cJSON_AddItemToArray(dht_list, dnode);
            dht_added++;
        }
    }
    cJSON_AddItemToObject(root, "dht_nodes", dht_list);

    cJSON *hist = cJSON_CreateArray();
    for (size_t h = 0; h < srv->history_count; h++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "type", "message");
        cJSON_AddStringToObject(m, "author", srv->history[h].author);

        char apk_hex[65];
        wex_id_to_hex(srv->history[h].author_pk, apk_hex);
        cJSON_AddStringToObject(m, "author_pk", apk_hex);

        bool h_is_dm = (strncmp(srv->history[h].channel, "dm:", 3) == 0);
        bool h_encrypted = ((srv->history[h].enc_flags & WEX_ENC_DM_E2EE) != 0) || h_is_dm;

        cJSON_AddStringToObject(m, "channel", srv->history[h].channel);
        cJSON_AddStringToObject(m, "content", srv->history[h].content);
        cJSON_AddBoolToObject(m, "verified", true);
        cJSON_AddBoolToObject(m, "encrypted", h_encrypted);
        cJSON_AddNumberToObject(m, "timestamp_ms", (double)srv->history[h].timestamp_ms);

        char sig_hex[129];
        for (int i = 0; i < 64; i++) sprintf(sig_hex + (i * 2), "%02x", srv->history[h].signature[i]);
        sig_hex[128] = '\0';
        cJSON_AddStringToObject(m, "signature", sig_hex);

        char reacts[128];
        wex_reactions_get_summary(&srv->reactions, srv->history[h].signature, reacts, sizeof(reacts));
        cJSON_AddStringToObject(m, "reactions", reacts);

        cJSON_AddItemToArray(hist, m);
    }
    cJSON_AddItemToObject(root, "history", hist);

    cJSON *voice = cJSON_CreateObject();
    cJSON_AddBoolToObject(voice, "connected", srv->voice_room.is_in_call);
    cJSON_AddStringToObject(voice, "channel", srv->voice_room.channel);
    cJSON_AddItemToObject(root, "voice", voice);

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
    } else if (strcmp(action->valuestring, "add_contact") == 0 ||
               strcmp(action->valuestring, "connect_peer") == 0 ||
               strcmp(action->valuestring, "join_invite") == 0) {
        cJSON *target = cJSON_GetObjectItem(root, "target");
        if (!target) target = cJSON_GetObjectItem(root, "uri");
        if (!target) target = cJSON_GetObjectItem(root, "public_key");
        if (!target) target = cJSON_GetObjectItem(root, "pk");
        if (target && cJSON_IsString(target) && strlen(target->valuestring) > 0) {
            const char *t_str = target->valuestring;
            while (*t_str == ' ' || *t_str == '\t') t_str++;
            wex_p2p_join_invite(&srv->p2p, t_str);
            if (strlen(t_str) == 64 && strchr(t_str, ':') == NULL) {
                uint8_t pk[32];
                if (wex_hex_to_id(t_str, pk) == 0) {
                    wex_p2p_connect_by_pk(&srv->p2p, pk);
                    for (size_t i = 0; i < srv->p2p.peer_count; i++) {
                        if (srv->p2p.peers[i].active) {
                            wex_p2p_pull_offline_msgs(&srv->p2p, &srv->p2p.peers[i].addr);
                        }
                    }
                }
            }
            send_status_json(srv, client_fd);
        } else {
            cJSON *ip = cJSON_GetObjectItem(root, "ip");
            cJSON *port = cJSON_GetObjectItem(root, "port");
            if (ip && cJSON_IsString(ip) && port && cJSON_IsNumber(port)) {
                wex_server_connect_peer(srv, ip->valuestring, port->valueint);
                send_status_json(srv, client_fd);
            } else if (ip && cJSON_IsString(ip) && strlen(ip->valuestring) > 0) {
                wex_p2p_join_invite(&srv->p2p, ip->valuestring);
                send_status_json(srv, client_fd);
            }
        }
    } else if (strcmp(action->valuestring, "switch_space") == 0) {
        cJSON *sp_id = cJSON_GetObjectItem(root, "space_id");
        if (sp_id && cJSON_IsString(sp_id) && strlen(sp_id->valuestring) == 64) {
            uint8_t sid[32];
            if (wex_hex_to_id(sp_id->valuestring, sid) == 0) {
                wex_p2p_set_active_space(&srv->p2p, sid);
                send_status_json(srv, client_fd);
            }
        }
    } else if (strcmp(action->valuestring, "set_profile") == 0) {
        cJSON *nick = cJSON_GetObjectItem(root, "nickname");
        cJSON *status = cJSON_GetObjectItem(root, "status");
        if (nick && cJSON_IsString(nick) && strlen(nick->valuestring) > 0) {
            strncpy(srv->identity.nickname, nick->valuestring, sizeof(srv->identity.nickname) - 1);
            srv->identity.nickname[sizeof(srv->identity.nickname) - 1] = '\0';
        }
        if (status && cJSON_IsString(status) && strlen(status->valuestring) > 0) {
            strncpy(srv->identity.status, status->valuestring, sizeof(srv->identity.status) - 1);
            srv->identity.status[sizeof(srv->identity.status) - 1] = '\0';
        }
        const char *dir = (strlen(srv->p2p.data_dir) > 0) ? srv->p2p.data_dir : "./.wex_data";
        wex_mkdir_p(dir);
        char key_path[560];
        snprintf(key_path, sizeof(key_path), "%s/identity.key", dir);
        wex_identity_save(&srv->identity, key_path);
        send_status_json(srv, client_fd);
    } else if (strcmp(action->valuestring, "export_secret") == 0) {
        char seed_hex[65];
        char pk_hex[65];
        wex_id_to_hex(srv->identity.seed, seed_hex);
        wex_id_to_hex(srv->identity.sign_pk, pk_hex);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "type", "secret_exported");
        cJSON_AddStringToObject(resp, "seed_hex", seed_hex);
        cJSON_AddStringToObject(resp, "public_key", pk_hex);
        char *resp_str = cJSON_PrintUnformatted(resp);
        if (resp_str) {
            send_ws_frame(client_fd, resp_str, strlen(resp_str));
            free(resp_str);
        }
        cJSON_Delete(resp);
    } else if (strcmp(action->valuestring, "import_secret") == 0) {
        cJSON *seed_json = cJSON_GetObjectItem(root, "seed_hex");
        if (!seed_json) seed_json = cJSON_GetObjectItem(root, "seed");
        if (seed_json && cJSON_IsString(seed_json) && strlen(seed_json->valuestring) == 64) {
            uint8_t seed[32];
            if (wex_hex_to_id(seed_json->valuestring, seed) == 0) {
                char current_nick[WEX_NAME_LEN];
                char current_status[32];
                strncpy(current_nick, srv->identity.nickname, sizeof(current_nick) - 1);
                current_nick[sizeof(current_nick) - 1] = '\0';
                strncpy(current_status, srv->identity.status, sizeof(current_status) - 1);
                current_status[sizeof(current_status) - 1] = '\0';

                if (wex_identity_init(&srv->identity, seed, current_nick) == 0) {
                    if (strlen(current_status) > 0) {
                        strncpy(srv->identity.status, current_status, sizeof(srv->identity.status) - 1);
                        srv->identity.status[sizeof(srv->identity.status) - 1] = '\0';
                    }
                    memcpy(srv->p2p.dht.local_id, srv->identity.sign_pk, 32);
                    const char *dir = (strlen(srv->p2p.data_dir) > 0) ? srv->p2p.data_dir : "./.wex_data";
                    wex_mkdir_p(dir);
                    char key_path[560];
                    snprintf(key_path, sizeof(key_path), "%s/identity.key", dir);
                    wex_identity_save(&srv->identity, key_path);
                    send_status_json(srv, client_fd);
                }
                wex_clean(seed, sizeof(seed));
            }
        }
    } else if (strcmp(action->valuestring, "lan_beacon") == 0) {
        wex_p2p_send_lan_beacon(&srv->p2p);
        send_status_json(srv, client_fd);
    } else if (strcmp(action->valuestring, "stun_probe") == 0) {
        struct sockaddr_in ext_addr;
        if (wex_nat_auto_discover(srv->p2p.udp_fd, &ext_addr) == 0) {
            srv->p2p.external_addr = ext_addr;
            srv->p2p.has_external_addr = true;
        }
        send_status_json(srv, client_fd);
    } else if (strcmp(action->valuestring, "pull_offline") == 0) {
        for (size_t i = 0; i < srv->p2p.peer_count; i++) {
            if (srv->p2p.peers[i].active) {
                wex_p2p_pull_offline_msgs(&srv->p2p, &srv->p2p.peers[i].addr);
            }
        }
        send_status_json(srv, client_fd);
    } else if (strcmp(action->valuestring, "create_space") == 0) {
        cJSON *name = cJSON_GetObjectItem(root, "name");
        if (name && cJSON_IsString(name) && srv->space_count < WEX_MAX_SPACES) {
            wex_space_create(&srv->spaces[srv->space_count], name->valuestring);
            wex_p2p_add_space(&srv->p2p, &srv->spaces[srv->space_count]);
            wex_p2p_set_active_space(&srv->p2p, srv->spaces[srv->space_count].space_id);
            srv->space_count++;
            send_status_json(srv, client_fd);
        }
    } else if (strcmp(action->valuestring, "create_channel") == 0) {
        cJSON *ch_name = cJSON_GetObjectItem(root, "name");
        if (ch_name && cJSON_IsString(ch_name)) {
            wex_p2p_create_channel(&srv->p2p, ch_name->valuestring);
            send_status_json(srv, client_fd);
        }
    } else if (strcmp(action->valuestring, "switch_channel") == 0) {
        cJSON *ch_name = cJSON_GetObjectItem(root, "name");
        if (ch_name && cJSON_IsString(ch_name)) {
            wex_p2p_set_active_channel(&srv->p2p, ch_name->valuestring);
            send_status_json(srv, client_fd);
        }
    } else if (strcmp(action->valuestring, "send_message") == 0) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        cJSON *channel = cJSON_GetObjectItem(root, "channel");
        if (text && cJSON_IsString(text)) {
            wex_chat_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            if (srv->p2p.space_count > 0) {
                memcpy(msg.space_id, srv->p2p.active_space_id, WEX_ID_LEN);
            } else if (srv->space_count > 0) {
                memcpy(msg.space_id, srv->spaces[0].space_id, WEX_ID_LEN);
            }
            if (channel && cJSON_IsString(channel) && strlen(channel->valuestring) > 0) {
                strncpy(msg.channel, channel->valuestring, sizeof(msg.channel) - 1);
            } else if (strlen(srv->p2p.active_channel) > 0) {
                strncpy(msg.channel, srv->p2p.active_channel, sizeof(msg.channel) - 1);
            } else {
                strncpy(msg.channel, "general", sizeof(msg.channel) - 1);
            }
            strncpy(msg.content, text->valuestring, sizeof(msg.content) - 1);

            /* Sign message with Ed25519 identity */
            wex_chat_msg_sign(&msg, &srv->identity);

            /* Save to local history */
            if (srv->history_count < WEX_MAX_HISTORY) {
                srv->history[srv->history_count++] = msg;
            }

            if (srv->journal && srv->p2p.journal != srv->journal) {
                wex_journal_append(srv->journal, &msg);
            }

            char my_pk_hex[65];
            wex_id_to_hex(srv->identity.sign_pk, my_pk_hex);
            char sig_hex[129];
            for (int i = 0; i < 64; i++) sprintf(sig_hex + (i * 2), "%02x", msg.signature[i]);
            sig_hex[128] = '\0';

            /* 1. Broadcast locally to WebSockets */
            cJSON *out = cJSON_CreateObject();
            cJSON_AddStringToObject(out, "type", "message");
            cJSON_AddStringToObject(out, "author", msg.author);
            cJSON_AddStringToObject(out, "author_pk", my_pk_hex);
            cJSON_AddStringToObject(out, "channel", msg.channel);
            cJSON_AddStringToObject(out, "content", msg.content);
            cJSON_AddBoolToObject(out, "verified", true);
            cJSON_AddBoolToObject(out, "encrypted", false);
            cJSON_AddStringToObject(out, "signature", sig_hex);
            cJSON_AddNumberToObject(out, "timestamp_ms", (double)msg.timestamp_ms);
            cJSON_AddStringToObject(out, "reactions", "");

            char *json_out = cJSON_PrintUnformatted(out);
            if (json_out) {
                wex_server_broadcast(srv, json_out);
                free(json_out);
            }
            cJSON_Delete(out);

            /* 2. Broadcast over direct 1-hop UDP peers */
            int sent_direct = wex_p2p_broadcast_msg(&srv->p2p, &msg);

            /* 3. DHT Overlay Fallback:
             * If target peer or topic providers are not directly connected,
             * resolve route via Kademlia DHT and dispatch via wex_p2p_relay_msg! */
            cJSON *target_hex = cJSON_GetObjectItem(root, "target_id");
            if (target_hex && cJSON_IsString(target_hex) && strlen(target_hex->valuestring) == 64) {
                uint8_t target_pk[32];
                if (wex_hex_to_id(target_hex->valuestring, target_pk) == 0) {
                    wex_p2p_relay_msg(&srv->p2p, target_pk, &msg);
                }
            } else {
                /* Lookup topic providers in Kademlia DHT */
                wex_dht_provider_t provs[WEX_DHT_MAX_PROVIDERS];
                size_t prov_count = wex_dht_get_topic_providers(&srv->p2p.dht, msg.space_id, provs, WEX_DHT_MAX_PROVIDERS);
                for (size_t p = 0; p < prov_count; p++) {
                    bool is_direct = false;
                    for (size_t d = 0; d < srv->p2p.peer_count; d++) {
                        if (srv->p2p.peers[d].active && memcmp(srv->p2p.peers[d].pk, provs[p].peer_id, 32) == 0) {
                            is_direct = true;
                            break;
                        }
                    }
                    if (!is_direct && memcmp(provs[p].peer_id, srv->identity.sign_pk, 32) != 0) {
                        wex_p2p_relay_msg(&srv->p2p, provs[p].peer_id, &msg);
                    }
                }

                /* If no direct 1-hop peer was reached, route through DHT next-hop toward space_id */
                if (sent_direct == 0 && prov_count == 0 && srv->p2p.dht.total_contacts > 0) {
                    wex_p2p_relay_msg(&srv->p2p, msg.space_id, &msg);
                }
            }
        }
    } else if (strcmp(action->valuestring, "send_dm") == 0) {
        cJSON *target_pk_json = cJSON_GetObjectItem(root, "target_pk");
        if (!target_pk_json) target_pk_json = cJSON_GetObjectItem(root, "peer_pk");
        if (!target_pk_json) target_pk_json = cJSON_GetObjectItem(root, "to");
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (!text) text = cJSON_GetObjectItem(root, "content");
        if (target_pk_json && cJSON_IsString(target_pk_json) && text && cJSON_IsString(text)) {
            uint8_t target_pk[32];
            if (wex_hex_to_id(target_pk_json->valuestring, target_pk) == 0) {
                wex_chat_msg_t local_msg;
                memset(&local_msg, 0, sizeof(local_msg));
                wex_p2p_send_dm_ex(&srv->p2p, target_pk, text->valuestring, &local_msg);

                /* Broadcast to local WebSocket UI */
                char target_hex[65];
                char my_pk_hex[65];
                wex_id_to_hex(target_pk, target_hex);
                wex_id_to_hex(srv->identity.sign_pk, my_pk_hex);
                local_msg.enc_flags = WEX_ENC_DM_E2EE;

                if (srv->history_count < WEX_MAX_HISTORY) {
                    srv->history[srv->history_count++] = local_msg;
                }

                if (srv->journal && srv->p2p.journal != srv->journal) {
                    wex_journal_append(srv->journal, &local_msg);
                }

                char sig_hex[129];
                for (int i = 0; i < 64; i++) sprintf(sig_hex + (i * 2), "%02x", local_msg.signature[i]);
                sig_hex[128] = '\0';

                cJSON *out = cJSON_CreateObject();
                cJSON_AddStringToObject(out, "type", "message");
                cJSON_AddStringToObject(out, "author", srv->identity.nickname);
                cJSON_AddStringToObject(out, "author_pk", my_pk_hex);
                cJSON_AddStringToObject(out, "peer_pk", target_hex);
                cJSON_AddStringToObject(out, "channel", local_msg.channel);
                cJSON_AddStringToObject(out, "content", local_msg.content);
                cJSON_AddBoolToObject(out, "verified", true);
                cJSON_AddBoolToObject(out, "encrypted", true);
                cJSON_AddStringToObject(out, "signature", sig_hex);
                cJSON_AddNumberToObject(out, "timestamp_ms", (double)local_msg.timestamp_ms);
                cJSON_AddStringToObject(out, "reactions", "");
                char *json_out = cJSON_PrintUnformatted(out);
                if (json_out) {
                    wex_server_broadcast(srv, json_out);
                    free(json_out);
                }
                cJSON_Delete(out);
            }
        }
    } else if (strcmp(action->valuestring, "upload_blob") == 0) {
        cJSON *fname = cJSON_GetObjectItem(root, "filename");
        cJSON *mtype = cJSON_GetObjectItem(root, "mime_type");
        cJSON *b64_data = cJSON_GetObjectItem(root, "data_base64");
        if (b64_data && cJSON_IsString(b64_data)) {
            size_t data_len = 0;
            uint8_t *bin = wex_b64_decode(b64_data->valuestring, &data_len);
            if (bin) {
                const char *fn = (fname && cJSON_IsString(fname)) ? fname->valuestring : "attachment.bin";
                const char *mt = (mtype && cJSON_IsString(mtype)) ? mtype->valuestring : "application/octet-stream";

                wex_blob_info_t info;
                if (wex_blob_info_create(&info, fn, mt, bin, data_len) == 0) {
                    wex_blob_save(srv->p2p.data_dir, &info, bin);
                    wex_p2p_offer_blob(&srv->p2p, &info);

                    char hash_hex[65];
                    wex_id_to_hex(info.root_hash, hash_hex);

                    /* Send chat message containing the attachment tag */
                    char attach_msg[512];
                    snprintf(attach_msg, sizeof(attach_msg), "[attachment:%s:%s:%llu:%s]",
                             hash_hex, info.filename, (unsigned long long)info.total_size, info.mime_type);

                    wex_chat_msg_t msg;
                    memset(&msg, 0, sizeof(msg));
                    memcpy(msg.space_id, srv->p2p.active_space_id, WEX_ID_LEN);
                    strncpy(msg.channel, srv->p2p.active_channel, sizeof(msg.channel) - 1);
                    strncpy(msg.content, attach_msg, sizeof(msg.content) - 1);
                    wex_chat_msg_sign(&msg, &srv->identity);

                    if (srv->history_count < WEX_MAX_HISTORY) {
                        srv->history[srv->history_count++] = msg;
                    }

                    cJSON *out = cJSON_CreateObject();
                    cJSON_AddStringToObject(out, "type", "message");
                    cJSON_AddStringToObject(out, "author", msg.author);
                    cJSON_AddStringToObject(out, "channel", msg.channel);
                    cJSON_AddStringToObject(out, "content", msg.content);
                    cJSON_AddNumberToObject(out, "timestamp_ms", (double)msg.timestamp_ms);
                    char *json_out = cJSON_PrintUnformatted(out);
                    if (json_out) {
                        wex_server_broadcast(srv, json_out);
                        free(json_out);
                    }
                    cJSON_Delete(out);

                    wex_p2p_broadcast_msg(&srv->p2p, &msg);

                    /* Send confirmation */
                    cJSON *ack = cJSON_CreateObject();
                    cJSON_AddStringToObject(ack, "type", "blob_uploaded");
                    cJSON_AddStringToObject(ack, "hash", hash_hex);
                    cJSON_AddStringToObject(ack, "filename", info.filename);
                    cJSON_AddNumberToObject(ack, "size", (double)info.total_size);
                    char *ack_str = cJSON_PrintUnformatted(ack);
                    if (ack_str) {
                        send_ws_frame(client_fd, ack_str, strlen(ack_str));
                        free(ack_str);
                    }
                    cJSON_Delete(ack);
                }
                free(bin);
            }
        }
    } else if (strcmp(action->valuestring, "get_blob") == 0) {
        cJSON *hash_json = cJSON_GetObjectItem(root, "hash");
        if (hash_json && cJSON_IsString(hash_json) && strlen(hash_json->valuestring) == 64) {
            uint8_t root_hash[32];
            if (wex_hex_to_id(hash_json->valuestring, root_hash) == 0) {
                uint8_t *blob_data = NULL;
                size_t blob_sz = 0;
                if (wex_blob_load(srv->p2p.data_dir, root_hash, &blob_data, &blob_sz) == 0) {
                    char *b64 = wex_b64_encode(blob_data, blob_sz);
                    if (b64) {
                        cJSON *resp = cJSON_CreateObject();
                        cJSON_AddStringToObject(resp, "type", "blob_data");
                        cJSON_AddStringToObject(resp, "hash", hash_json->valuestring);
                        cJSON_AddStringToObject(resp, "data_base64", b64);
                        cJSON_AddNumberToObject(resp, "size", (double)blob_sz);
                        char *resp_str = cJSON_PrintUnformatted(resp);
                        if (resp_str) {
                            send_ws_frame(client_fd, resp_str, strlen(resp_str));
                            free(resp_str);
                        }
                        cJSON_Delete(resp);
                        free(b64);
                    }
                    free(blob_data);
                } else {
                    /* Not on disk - auto request download from first available peer */
                    wex_blob_info_t req_info;
                    memset(&req_info, 0, sizeof(req_info));
                    memcpy(req_info.root_hash, root_hash, 32);
                    req_info.total_chunks = 1;
                    for (size_t p = 0; p < srv->p2p.peer_count; p++) {
                        if (srv->p2p.peers[p].active) {
                            wex_p2p_download_blob(&srv->p2p, &srv->p2p.peers[p].addr, &req_info);
                            break;
                        }
                    }
                }
            }
        }
    } else if (strcmp(action->valuestring, "send_reaction") == 0) {
        cJSON *sig_hex = cJSON_GetObjectItem(root, "target_sig");
        cJSON *emoji = cJSON_GetObjectItem(root, "emoji");
        if (sig_hex && cJSON_IsString(sig_hex) && strlen(sig_hex->valuestring) == 128 &&
            emoji && cJSON_IsString(emoji)) {
            uint8_t tgt_sig[64];
            for (int i = 0; i < 64; i++) {
                unsigned int v;
                sscanf(sig_hex->valuestring + (i * 2), "%02x", &v);
                tgt_sig[i] = (uint8_t)v;
            }
            wex_chat_msg_t *target_msg = NULL;
            for (size_t h = 0; h < srv->history_count; h++) {
                if (memcmp(srv->history[h].signature, tgt_sig, 64) == 0) {
                    target_msg = &srv->history[h];
                    break;
                }
            }
            if (target_msg) {
                wex_msg_action_t act;
                memset(&act, 0, sizeof(act));
                act.action_type = WEX_ACTION_REACT;
                memcpy(act.target_sig, tgt_sig, 64);
                memcpy(act.space_id, target_msg->space_id, 32);
                strncpy(act.channel, target_msg->channel, sizeof(act.channel) - 1);
                strncpy(act.payload, emoji->valuestring, sizeof(act.payload) - 1);
                act.timestamp_ms = (uint64_t)time(NULL) * 1000ULL;
                wex_msg_action_sign(&act, &srv->identity);

                if (wex_msg_action_apply(&srv->reactions, NULL, target_msg, &act) == 0) {
                    wex_p2p_send_action(&srv->p2p, act.space_id, &act);
                    char summary[128];
                    wex_reactions_get_summary(&srv->reactions, tgt_sig, summary, sizeof(summary));

                    cJSON *out = cJSON_CreateObject();
                    cJSON_AddStringToObject(out, "type", "reaction_updated");
                    cJSON_AddStringToObject(out, "target_sig", sig_hex->valuestring);
                    cJSON_AddStringToObject(out, "summary", summary);
                    char *json_out = cJSON_PrintUnformatted(out);
                    if (json_out) {
                        wex_server_broadcast(srv, json_out);
                        free(json_out);
                    }
                    cJSON_Delete(out);
                }
            }
        }
    } else if (strcmp(action->valuestring, "edit_message") == 0) {
        cJSON *sig_hex = cJSON_GetObjectItem(root, "target_sig");
        cJSON *new_text = cJSON_GetObjectItem(root, "text");
        if (sig_hex && cJSON_IsString(sig_hex) && strlen(sig_hex->valuestring) == 128 &&
            new_text && cJSON_IsString(new_text)) {
            uint8_t tgt_sig[64];
            for (int i = 0; i < 64; i++) {
                unsigned int v;
                sscanf(sig_hex->valuestring + (i * 2), "%02x", &v);
                tgt_sig[i] = (uint8_t)v;
            }
            for (size_t h = 0; h < srv->history_count; h++) {
                if (memcmp(srv->history[h].signature, tgt_sig, 64) == 0) {
                    wex_msg_action_t act;
                    memset(&act, 0, sizeof(act));
                    act.action_type = WEX_ACTION_EDIT;
                    memcpy(act.target_sig, tgt_sig, 64);
                    memcpy(act.space_id, srv->history[h].space_id, 32);
                    strncpy(act.channel, srv->history[h].channel, sizeof(act.channel) - 1);
                    strncpy(act.payload, new_text->valuestring, sizeof(act.payload) - 1);
                    act.timestamp_ms = (uint64_t)time(NULL) * 1000ULL;
                    wex_msg_action_sign(&act, &srv->identity);

                    if (wex_msg_action_apply(&srv->reactions, NULL, &srv->history[h], &act) == 0) {
                        wex_p2p_send_action(&srv->p2p, act.space_id, &act);
                        cJSON *out = cJSON_CreateObject();
                        cJSON_AddStringToObject(out, "type", "message_edited");
                        cJSON_AddStringToObject(out, "target_sig", sig_hex->valuestring);
                        cJSON_AddStringToObject(out, "content", srv->history[h].content);
                        char *json_out = cJSON_PrintUnformatted(out);
                        if (json_out) {
                            wex_server_broadcast(srv, json_out);
                            free(json_out);
                        }
                        cJSON_Delete(out);
                    }
                    break;
                }
            }
        }
    } else if (strcmp(action->valuestring, "delete_message") == 0) {
        cJSON *sig_hex = cJSON_GetObjectItem(root, "target_sig");
        if (sig_hex && cJSON_IsString(sig_hex) && strlen(sig_hex->valuestring) == 128) {
            uint8_t tgt_sig[64];
            for (int i = 0; i < 64; i++) {
                unsigned int v;
                sscanf(sig_hex->valuestring + (i * 2), "%02x", &v);
                tgt_sig[i] = (uint8_t)v;
            }
            for (size_t h = 0; h < srv->history_count; h++) {
                if (memcmp(srv->history[h].signature, tgt_sig, 64) == 0) {
                    wex_msg_action_t act;
                    memset(&act, 0, sizeof(act));
                    act.action_type = WEX_ACTION_DELETE;
                    memcpy(act.target_sig, tgt_sig, 64);
                    memcpy(act.space_id, srv->history[h].space_id, 32);
                    strncpy(act.channel, srv->history[h].channel, sizeof(act.channel) - 1);
                    act.timestamp_ms = (uint64_t)time(NULL) * 1000ULL;
                    wex_msg_action_sign(&act, &srv->identity);

                    if (wex_msg_action_apply(&srv->reactions, NULL, &srv->history[h], &act) == 0) {
                        wex_p2p_send_action(&srv->p2p, act.space_id, &act);
                        if (h + 1 < srv->history_count) {
                            memmove(&srv->history[h], &srv->history[h + 1],
                                    (srv->history_count - h - 1) * sizeof(wex_chat_msg_t));
                        }
                        srv->history_count--;
                        cJSON *out = cJSON_CreateObject();
                        cJSON_AddStringToObject(out, "type", "message_deleted");
                        cJSON_AddStringToObject(out, "target_sig", sig_hex->valuestring);
                        char *json_out = cJSON_PrintUnformatted(out);
                        if (json_out) {
                            wex_server_broadcast(srv, json_out);
                            free(json_out);
                        }
                        cJSON_Delete(out);
                    }
                    break;
                }
            }
        }
    } else if (strcmp(action->valuestring, "join_voice") == 0) {
        cJSON *vch = cJSON_GetObjectItem(root, "channel");
        const char *vc = (vch && cJSON_IsString(vch)) ? vch->valuestring : "voice-lounge";
        wex_voice_room_init(&srv->voice_room, vc, (uint32_t)srv->udp_port);
        srv->voice_room.is_in_call = true;
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "type", "voice_state");
        cJSON_AddBoolToObject(out, "connected", true);
        cJSON_AddStringToObject(out, "channel", srv->voice_room.channel);
        char *json_out = cJSON_PrintUnformatted(out);
        if (json_out) {
            wex_server_broadcast(srv, json_out);
            free(json_out);
        }
        cJSON_Delete(out);
    } else if (strcmp(action->valuestring, "leave_voice") == 0) {
        srv->voice_room.is_in_call = false;
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "type", "voice_state");
        cJSON_AddBoolToObject(out, "connected", false);
        cJSON_AddStringToObject(out, "channel", "");
        char *json_out = cJSON_PrintUnformatted(out);
        if (json_out) {
            wex_server_broadcast(srv, json_out);
            free(json_out);
        }
        cJSON_Delete(out);
    } else if (strcmp(action->valuestring, "voice_data") == 0) {
        cJSON *b64 = cJSON_GetObjectItem(root, "data_base64");
        if (b64 && cJSON_IsString(b64) && srv->voice_room.is_in_call) {
            size_t audio_len = 0;
            uint8_t *audio_bytes = wex_b64_decode(b64->valuestring, &audio_len);
            if (audio_bytes && audio_len > 0) {
                wex_voice_hdr_t vhdr;
                wex_voice_create_header(&srv->voice_room, &vhdr, audio_len);

                wex_p2p_send_voice_frame(&srv->p2p, srv->p2p.active_space_id, srv->voice_room.channel, &vhdr, audio_bytes, audio_len);
                free(audio_bytes);
            }
        }
    }

    cJSON_Delete(root);
}

static bool perform_handshake(int fd, const char *req, size_t req_len) {
    (void)req_len;
    const char *key_hdr = "Sec-WebSocket-Key: ";
    const char *key_start = strstr(req, key_hdr);

    if (!key_start) {
        if (strstr(req, "GET ") == req) {
            if (strstr(req, "GET /qr_engine.js") == req) {
                serve_static_file(fd, "web/qr_engine.js", "application/javascript; charset=UTF-8");
            } else if (strstr(req, "GET /wex_app.js") == req) {
                serve_static_file(fd, "web/wex_app.js", "application/javascript; charset=UTF-8");
            } else {
                serve_html_file(fd);
            }
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

    char concat[256];
    snprintf(concat, sizeof(concat), "%s%s", client_key, WS_GUID);

    wex_sha1_ctx sha_ctx;
    uint8_t digest[20];
    wex_sha1_init(&sha_ctx);
    wex_sha1_update(&sha_ctx, (uint8_t *)concat, strlen(concat));
    wex_sha1_final(&sha_ctx, digest);

    char accept_key[64];
    wex_base64_encode(digest, 20, accept_key);

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
    if (!srv || srv->ws_server_fd < 0) return -1;

    /* Always poll P2P UDP engine for incoming peer datagrams */
    wex_p2p_poll(&srv->p2p, 0);

    struct pollfd fds[WEX_MAX_CLIENTS + 1];
    int count = 0;

    fds[0].fd = srv->ws_server_fd;
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

    /* Check new incoming WebSocket connection */
    if (fds[0].revents & POLLIN) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int new_fd = accept(srv->ws_server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (new_fd >= 0) {
            set_nonblocking(new_fd);

            char buf[4096];
            usleep(10000);
            ssize_t n = read(new_fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                if (perform_handshake(new_fd, buf, (size_t)n)) {
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

    /* Check data from existing WebSocket clients */
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

            if (r < 2) continue;
            uint8_t opcode = frame_buf[0] & 0x0F;
            if (opcode == 0x08) {
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
    if (srv->ws_server_fd >= 0) {
        close(srv->ws_server_fd);
        srv->ws_server_fd = -1;
    }
    wex_p2p_close(&srv->p2p);
    srv->running = false;
}
