#define _GNU_SOURCE
#include "wex_p2p.h"
#include "wex_crypto.h"
#include "wex_nat.h"
#include "../../vendor/monocypher/monocypher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

int wex_p2p_init(wex_p2p_node_t *node, wex_identity_t *ident, int udp_port) {
    if (!node || !ident) return -1;
    memset(node, 0, sizeof(*node));
    node->identity = ident;
    node->port = udp_port;
    node->udp_fd = -1;
    strncpy(node->active_channel, "general", sizeof(node->active_channel) - 1);
    strncpy(node->data_dir, "./.wex_data", sizeof(node->data_dir) - 1);

    /* Initialize Kademlia DHT routing table */
    wex_dht_init(&node->dht, ident->sign_pk);

    node->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (node->udp_fd < 0) {
        perror("socket(SOCK_DGRAM)");
        return -1;
    }

    int opt = 1;
    setsockopt(node->udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons((uint16_t)udp_port);

    if (bind(node->udp_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind(UDP)");
        close(node->udp_fd);
        node->udp_fd = -1;
        return -1;
    }

    set_nonblock(node->udp_fd);
    node->running = true;

    printf("[+] Wex P2P UDP engine bound to port %d\n", udp_port);
    return 0;
}

void wex_p2p_set_callbacks(wex_p2p_node_t *node, wex_on_msg_fn msg_cb, wex_on_peer_fn peer_cb, void *user_data) {
    if (!node) return;
    node->msg_cb = msg_cb;
    node->peer_cb = peer_cb;
    node->user_data = user_data;
}

void wex_p2p_set_sync_callback(wex_p2p_node_t *node, wex_on_sync_req_fn sync_cb) {
    if (!node) return;
    node->sync_cb = sync_cb;
}

void wex_p2p_set_journal(wex_p2p_node_t *node, wex_journal_t *journal) {
    if (!node) return;
    node->journal = journal;
}

void wex_p2p_set_blob_callback(wex_p2p_node_t *node, wex_on_blob_fn blob_cb) {
    if (!node) return;
    node->blob_cb = blob_cb;
}

void wex_p2p_set_action_callback(wex_p2p_node_t *node, wex_on_action_fn action_cb) {
    if (!node) return;
    node->action_cb = action_cb;
}

void wex_p2p_set_voice_callback(wex_p2p_node_t *node, wex_on_voice_fn voice_cb) {
    if (!node) return;
    node->voice_cb = voice_cb;
}

void wex_p2p_set_data_dir(wex_p2p_node_t *node, const char *data_dir) {
    if (!node || !data_dir) return;
    strncpy(node->data_dir, data_dir, sizeof(node->data_dir) - 1);
    node->data_dir[sizeof(node->data_dir) - 1] = '\0';
}

int wex_p2p_offer_blob(wex_p2p_node_t *node, const wex_blob_info_t *info) {
    if (!node || node->udp_fd < 0 || !info) return -1;

    uint8_t buffer[sizeof(wex_pkt_hdr_t) + sizeof(wex_blob_info_t)];
    wex_pkt_hdr_t *hdr = (wex_pkt_hdr_t *)buffer;
    hdr->magic = htonl(WEX_P2P_MAGIC);
    hdr->type = WEX_PKT_BLOB_OFFER;
    memcpy(hdr->sender_pk, node->identity->sign_pk, 32);
    memcpy(hdr->space_id, node->active_space_id, 32);
    hdr->payload_len = htons((uint16_t)sizeof(wex_blob_info_t));
    memcpy(buffer + sizeof(wex_pkt_hdr_t), info, sizeof(wex_blob_info_t));

    int sent = 0;
    for (size_t i = 0; i < node->peer_count; i++) {
        if (node->peers[i].active) {
            ssize_t s = sendto(node->udp_fd, buffer, sizeof(buffer), 0,
                               (struct sockaddr *)&node->peers[i].addr, sizeof(node->peers[i].addr));
            if (s == (ssize_t)sizeof(buffer)) sent++;
        }
    }
    return sent;
}

int wex_p2p_request_blob_chunk(wex_p2p_node_t *node, const struct sockaddr_in *target_addr,
                               const uint8_t root_hash[32], uint32_t chunk_idx) {
    if (!node || node->udp_fd < 0 || !target_addr || !root_hash) return -1;

    uint8_t buffer[sizeof(wex_pkt_hdr_t) + 36];
    wex_pkt_hdr_t *hdr = (wex_pkt_hdr_t *)buffer;
    hdr->magic = htonl(WEX_P2P_MAGIC);
    hdr->type = WEX_PKT_BLOB_REQ;
    memcpy(hdr->sender_pk, node->identity->sign_pk, 32);
    memcpy(hdr->space_id, node->active_space_id, 32);
    hdr->payload_len = htons(36);

    uint8_t *payload = buffer + sizeof(wex_pkt_hdr_t);
    memcpy(payload, root_hash, 32);
    uint32_t net_idx = htonl(chunk_idx);
    memcpy(payload + 32, &net_idx, 4);

    ssize_t s = sendto(node->udp_fd, buffer, sizeof(buffer), 0,
                       (struct sockaddr *)target_addr, sizeof(*target_addr));
    return (s == (ssize_t)sizeof(buffer)) ? 0 : -1;
}

int wex_p2p_download_blob(wex_p2p_node_t *node, const struct sockaddr_in *peer_addr, const wex_blob_info_t *info) {
    if (!node || !peer_addr || !info) return -1;

    if (node->is_downloading) {
        wex_blob_assembler_free(&node->active_download);
    }

    if (wex_blob_assembler_init(&node->active_download, info) != 0) {
        return -1;
    }

    node->download_peer = *peer_addr;
    node->is_downloading = true;

    /* Request first chunk */
    return wex_p2p_request_blob_chunk(node, peer_addr, info->root_hash, 0);
}

int wex_p2p_send_blob_chunk(wex_p2p_node_t *node, const struct sockaddr_in *target_addr,
                            const uint8_t root_hash[32], uint32_t chunk_idx, uint32_t total_chunks,
                            const uint8_t *data, size_t len) {
    if (!node || node->udp_fd < 0 || !target_addr || !root_hash || len > WEX_BLOB_CHUNK_SIZE) return -1;

    uint8_t buffer[sizeof(wex_pkt_hdr_t) + sizeof(wex_blob_chunk_hdr_t) + WEX_BLOB_CHUNK_SIZE];
    wex_pkt_hdr_t *hdr = (wex_pkt_hdr_t *)buffer;
    hdr->magic = htonl(WEX_P2P_MAGIC);
    hdr->type = WEX_PKT_BLOB_CHUNK;
    memcpy(hdr->sender_pk, node->identity->sign_pk, 32);
    memcpy(hdr->space_id, node->active_space_id, 32);

    uint8_t *payload = buffer + sizeof(wex_pkt_hdr_t);
    wex_blob_chunk_hdr_t *chdr = (wex_blob_chunk_hdr_t *)payload;
    memcpy(chdr->root_hash, root_hash, 32);
    chdr->chunk_idx = htonl(chunk_idx);
    chdr->total_chunks = htonl(total_chunks);
    chdr->data_len = htons((uint16_t)len);

    if (len > 0 && data) {
        memcpy(payload + sizeof(wex_blob_chunk_hdr_t), data, len);
    }

    size_t payload_len = sizeof(wex_blob_chunk_hdr_t) + len;
    hdr->payload_len = htons((uint16_t)payload_len);

    ssize_t s = sendto(node->udp_fd, buffer, sizeof(wex_pkt_hdr_t) + payload_len, 0,
                       (struct sockaddr *)target_addr, sizeof(*target_addr));
    return (s == (ssize_t)(sizeof(wex_pkt_hdr_t) + payload_len)) ? 0 : -1;
}

wex_space_t *wex_p2p_get_active_space(wex_p2p_node_t *node) {
    if (!node) return NULL;
    for (size_t i = 0; i < node->space_count; i++) {
        if (memcmp(node->spaces[i].space_id, node->active_space_id, 32) == 0) {
            return &node->spaces[i];
        }
    }
    return NULL;
}

int wex_p2p_add_space(wex_p2p_node_t *node, const wex_space_t *space) {
    if (!node || !space) return -1;

    for (size_t i = 0; i < node->space_count; i++) {
        if (memcmp(node->spaces[i].space_id, space->space_id, 32) == 0) {
            node->spaces[i] = *space;
            return 0;
        }
    }

    if (node->space_count < WEX_MAX_SPACES) {
        node->spaces[node->space_count++] = *space;
        if (node->space_count == 1) {
            wex_p2p_set_active_space(node, space->space_id);
        }
        return 0;
    }
    return -1;
}

int wex_p2p_set_active_space(wex_p2p_node_t *node, const uint8_t space_id[32]) {
    if (!node || !space_id) return -1;
    memcpy(node->active_space_id, space_id, 32);

    wex_space_t *sp = wex_p2p_get_active_space(node);
    if (sp && sp->channel_count > 0) {
        strncpy(node->active_channel, sp->channels[0].name, sizeof(node->active_channel) - 1);
    } else {
        strncpy(node->active_channel, "general", sizeof(node->active_channel) - 1);
    }
    return 0;
}

int wex_p2p_set_active_channel(wex_p2p_node_t *node, const char *channel_name) {
    if (!node || !channel_name || strlen(channel_name) == 0) return -1;
    strncpy(node->active_channel, channel_name, sizeof(node->active_channel) - 1);
    node->active_channel[sizeof(node->active_channel) - 1] = '\0';
    return 0;
}

int wex_p2p_send_space_info(wex_p2p_node_t *node, const struct sockaddr_in *target_addr) {
    if (!node || node->udp_fd < 0 || !target_addr) return -1;

    wex_space_t *sp = wex_p2p_get_active_space(node);
    if (!sp) return -1;

    uint8_t buffer[WEX_MAX_UDP_PAYLOAD];
    wex_pkt_hdr_t *hdr = (wex_pkt_hdr_t *)buffer;
    hdr->magic = htonl(WEX_P2P_MAGIC);
    hdr->type = WEX_PKT_SPACE_INFO;
    memcpy(hdr->sender_pk, node->identity->sign_pk, 32);
    memcpy(hdr->space_id, sp->space_id, 32);

    uint8_t *payload = buffer + sizeof(wex_pkt_hdr_t);
    size_t off = 0;

    /* Space name */
    size_t name_len = strlen(sp->name) + 1;
    memcpy(payload + off, sp->name, name_len);
    off += name_len;

    /* Channel count */
    payload[off++] = (uint8_t)sp->channel_count;

    /* Channels */
    for (size_t c = 0; c < sp->channel_count; c++) {
        size_t ch_len = strlen(sp->channels[c].name) + 1;
        memcpy(payload + off, sp->channels[c].name, ch_len);
        off += ch_len;
    }

    hdr->payload_len = htons((uint16_t)off);
    ssize_t s = sendto(node->udp_fd, buffer, sizeof(wex_pkt_hdr_t) + off, 0,
                       (struct sockaddr *)target_addr, sizeof(*target_addr));
    return (s == (ssize_t)(sizeof(wex_pkt_hdr_t) + off)) ? 0 : -1;
}

int wex_p2p_create_channel(wex_p2p_node_t *node, const char *channel_name) {
    if (!node || !channel_name) return -1;

    wex_space_t *sp = wex_p2p_get_active_space(node);
    if (!sp) return -1;

    if (wex_space_add_channel(sp, channel_name) != 0) {
        return -1;
    }

    /* Broadcast space info with new channel to all peers */
    for (size_t i = 0; i < node->peer_count; i++) {
        if (node->peers[i].active) {
            wex_p2p_send_space_info(node, &node->peers[i].addr);
        }
    }
    return 0;
}

void wex_p2p_request_sync(wex_p2p_node_t *node, const struct sockaddr_in *target_addr) {
    if (!node || node->udp_fd < 0 || !target_addr) return;

    uint8_t buffer[256];
    wex_pkt_hdr_t *hdr = (wex_pkt_hdr_t *)buffer;
    hdr->magic = htonl(WEX_P2P_MAGIC);
    hdr->type = WEX_PKT_SYNC_REQ;
    memcpy(hdr->sender_pk, node->identity->sign_pk, 32);
    memcpy(hdr->space_id, node->active_space_id, 32);

    size_t ch_len = strlen(node->active_channel) + 1;
    hdr->payload_len = htons((uint16_t)ch_len);
    memcpy(buffer + sizeof(wex_pkt_hdr_t), node->active_channel, ch_len);

    sendto(node->udp_fd, buffer, sizeof(wex_pkt_hdr_t) + ch_len, 0,
           (struct sockaddr *)target_addr, sizeof(*target_addr));
}

static wex_peer_t *find_or_add_peer(wex_p2p_node_t *node, const struct sockaddr_in *addr, const uint8_t pk[32], const char *nickname, const uint8_t *x25519_pk) {
    for (size_t i = 0; i < node->peer_count; i++) {
        if (memcmp(node->peers[i].pk, pk, 32) == 0 ||
            (node->peers[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
             node->peers[i].addr.sin_port == addr->sin_port)) {
            node->peers[i].addr = *addr;
            memcpy(node->peers[i].pk, pk, 32);
            if (x25519_pk) {
                memcpy(node->peers[i].x25519_pk, x25519_pk, 32);
            }
            if (nickname && strlen(nickname) > 0) {
                strncpy(node->peers[i].nickname, nickname, sizeof(node->peers[i].nickname) - 1);
            }
            node->peers[i].last_seen_ms = get_time_ms();
            node->peers[i].active = true;
            return &node->peers[i];
        }
    }

    if (node->peer_count < WEX_MAX_PEERS) {
        wex_peer_t *p = &node->peers[node->peer_count++];
        p->addr = *addr;
        memcpy(p->pk, pk, 32);
        if (x25519_pk) {
            memcpy(p->x25519_pk, x25519_pk, 32);
        } else {
            crypto_eddsa_to_x25519(p->x25519_pk, pk);
        }
        if (nickname && strlen(nickname) > 0) {
            strncpy(p->nickname, nickname, sizeof(p->nickname) - 1);
        } else {
            strncpy(p->nickname, "Peer", sizeof(p->nickname) - 1);
        }
        p->last_seen_ms = get_time_ms();
        p->active = true;
        p->synced = false;
        return p;
    }
    return NULL;
}

int wex_p2p_connect(wex_p2p_node_t *node, const char *host_or_ip, int port) {
    if (!node || node->udp_fd < 0 || !host_or_ip) return -1;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host_or_ip, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }

    struct sockaddr_in target_addr;
    memcpy(&target_addr, res->ai_addr, sizeof(target_addr));
    freeaddrinfo(res);

    /* Send WEX_PKT_HELLO with x25519_pk and nickname */
    uint8_t buffer[sizeof(wex_pkt_hdr_t) + sizeof(wex_pkt_hello_t)];
    wex_pkt_hdr_t *hdr = (wex_pkt_hdr_t *)buffer;
    hdr->magic = htonl(WEX_P2P_MAGIC);
    hdr->type = WEX_PKT_HELLO;
    memcpy(hdr->sender_pk, node->identity->sign_pk, 32);
    memcpy(hdr->space_id, node->active_space_id, 32);

    wex_pkt_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    memcpy(hello.x25519_pk, node->identity->x25519_pk, 32);
    strncpy(hello.nickname, node->identity->nickname, sizeof(hello.nickname) - 1);

    hdr->payload_len = htons((uint16_t)sizeof(hello));
    memcpy(buffer + sizeof(wex_pkt_hdr_t), &hello, sizeof(hello));

    size_t total_len = sizeof(buffer);
    ssize_t sent = sendto(node->udp_fd, buffer, total_len, 0,
                          (struct sockaddr *)&target_addr, sizeof(target_addr));
    return (sent == (ssize_t)total_len) ? 0 : -1;
}

static size_t serialize_chat_msg(const wex_chat_msg_t *msg, uint8_t *out_buf, size_t max_buf) {
    if (!msg || !out_buf || max_buf < sizeof(wex_pkt_hdr_t) + 128) return 0;

    wex_pkt_hdr_t *hdr = (wex_pkt_hdr_t *)out_buf;
    hdr->magic = htonl(WEX_P2P_MAGIC);
    hdr->type = WEX_PKT_CHAT_MSG;
    memcpy(hdr->sender_pk, msg->author_pk, 32);
    memcpy(hdr->space_id, msg->space_id, 32);

    uint8_t *payload = out_buf + sizeof(wex_pkt_hdr_t);
    size_t off = 0;

    /* Channel */
    size_t ch_len = strlen(msg->channel) + 1;
    memcpy(payload + off, msg->channel, ch_len);
    off += ch_len;

    /* Author */
    size_t author_len = strlen(msg->author) + 1;
    memcpy(payload + off, msg->author, author_len);
    off += author_len;

    /* Timestamp */
    uint64_t ts_net = msg->timestamp_ms;
    memcpy(payload + off, &ts_net, sizeof(ts_net));
    off += sizeof(ts_net);

    /* Encryption metadata */
    payload[off++] = msg->enc_flags;
    if (msg->enc_flags != 0) {
        memcpy(payload + off, msg->nonce, 24);
        off += 24;
        memcpy(payload + off, msg->mac, 16);
        off += 16;
    }

    /* Content */
    size_t cont_len = strlen(msg->content) + 1;
    memcpy(payload + off, msg->content, cont_len);
    off += cont_len;

    /* Ed25519 signature */
    memcpy(payload + off, msg->signature, 64);
    off += 64;

    hdr->payload_len = htons((uint16_t)off);
    return sizeof(wex_pkt_hdr_t) + off;
}

int wex_p2p_broadcast_msg(wex_p2p_node_t *node, const wex_chat_msg_t *msg) {
    if (!node || node->udp_fd < 0 || !msg) return -1;

    /* Auto-persist to journal if available */
    if (node->journal) {
        wex_journal_append(node->journal, msg);
    }

    uint8_t buffer[WEX_MAX_UDP_PAYLOAD];
    size_t total_len = serialize_chat_msg(msg, buffer, sizeof(buffer));
    if (total_len == 0) return -1;

    int sent_count = 0;
    for (size_t i = 0; i < node->peer_count; i++) {
        if (node->peers[i].active) {
            ssize_t s = sendto(node->udp_fd, buffer, total_len, 0,
                               (struct sockaddr *)&node->peers[i].addr, sizeof(node->peers[i].addr));
            if (s == (ssize_t)total_len) {
                sent_count++;
            }
        }
    }

    return sent_count;
}

int wex_p2p_send_action(wex_p2p_node_t *node, const uint8_t space_id[32], const wex_msg_action_t *action) {
    if (!node || node->udp_fd < 0 || !action) return -1;
    uint8_t buffer[WEX_MAX_UDP_PAYLOAD];
    wex_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = htonl(WEX_P2P_MAGIC);
    hdr.type = WEX_PKT_MSG_ACTION;
    memcpy(hdr.sender_pk, node->identity->sign_pk, 32);
    if (space_id) memcpy(hdr.space_id, space_id, 32);
    hdr.payload_len = htons((uint16_t)sizeof(wex_msg_action_t));

    memcpy(buffer, &hdr, sizeof(hdr));
    memcpy(buffer + sizeof(hdr), action, sizeof(wex_msg_action_t));
    size_t total_len = sizeof(hdr) + sizeof(wex_msg_action_t);

    int sent_count = 0;
    for (size_t i = 0; i < node->peer_count; i++) {
        if (node->peers[i].active) {
            ssize_t s = sendto(node->udp_fd, buffer, total_len, 0,
                               (struct sockaddr *)&node->peers[i].addr, sizeof(node->peers[i].addr));
            if (s == (ssize_t)total_len) {
                sent_count++;
            }
        }
    }
    return sent_count;
}

int wex_p2p_send_voice_frame(wex_p2p_node_t *node, const uint8_t space_id[32], const char *channel,
                             const wex_voice_hdr_t *voice_hdr, const uint8_t *audio_data, size_t audio_len) {
    if (!node || node->udp_fd < 0 || !voice_hdr) return -1;
    if (audio_len > 1024) return -1;

    uint8_t buffer[WEX_MAX_UDP_PAYLOAD];
    wex_pkt_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = htonl(WEX_P2P_MAGIC);
    hdr.type = WEX_PKT_VOICE_FRAME;
    memcpy(hdr.sender_pk, node->identity->sign_pk, 32);
    if (space_id) memcpy(hdr.space_id, space_id, 32);

    char ch_buf[WEX_NAME_LEN];
    memset(ch_buf, 0, sizeof(ch_buf));
    if (channel) strncpy(ch_buf, channel, sizeof(ch_buf) - 1);

    uint16_t p_len = (uint16_t)(sizeof(ch_buf) + sizeof(wex_voice_hdr_t) + audio_len);
    hdr.payload_len = htons(p_len);

    size_t off = 0;
    memcpy(buffer + off, &hdr, sizeof(hdr)); off += sizeof(hdr);
    memcpy(buffer + off, ch_buf, sizeof(ch_buf)); off += sizeof(ch_buf);
    memcpy(buffer + off, voice_hdr, sizeof(wex_voice_hdr_t)); off += sizeof(wex_voice_hdr_t);
    if (audio_data && audio_len > 0) {
        memcpy(buffer + off, audio_data, audio_len); off += audio_len;
    }

    int sent_count = 0;
    for (size_t i = 0; i < node->peer_count; i++) {
        if (node->peers[i].active) {
            ssize_t s = sendto(node->udp_fd, buffer, off, 0,
                               (struct sockaddr *)&node->peers[i].addr, sizeof(node->peers[i].addr));
            if (s == (ssize_t)off) {
                sent_count++;
            }
        }
    }
    return sent_count;
}

int wex_p2p_relay_msg(wex_p2p_node_t *node, const uint8_t dst_id[32], const wex_chat_msg_t *msg) {
    if (!node || node->udp_fd < 0 || !dst_id || !msg) return -1;

    uint8_t buffer[WEX_MAX_UDP_PAYLOAD];
    wex_dht_relay_hdr_t *relay_hdr = (wex_dht_relay_hdr_t *)buffer;
    relay_hdr->magic = htonl(WEX_P2P_MAGIC);
    relay_hdr->type = WEX_PKT_OVERLAY_RELAY;
    memcpy(relay_hdr->src_id, node->identity->sign_pk, 32);
    memcpy(relay_hdr->dst_id, dst_id, 32);
    memcpy(relay_hdr->topic_id, msg->space_id, 32);
    relay_hdr->hops_left = WEX_DHT_MAX_HOPS;

    uint8_t *payload = buffer + sizeof(wex_dht_relay_hdr_t);
    size_t inner_len = serialize_chat_msg(msg, payload, sizeof(buffer) - sizeof(wex_dht_relay_hdr_t));
    if (inner_len == 0) return -1;

    relay_hdr->payload_len = htons((uint16_t)inner_len);
    size_t total_len = sizeof(wex_dht_relay_hdr_t) + inner_len;

    wex_dht_contact_t next_hop;
    if (!wex_dht_next_hop(&node->dht, dst_id, &next_hop)) {
        if (wex_dht_find_closest(&node->dht, dst_id, &next_hop, 1) == 0) {
            return -2;
        }
    }

    ssize_t s = sendto(node->udp_fd, buffer, total_len, 0,
                       (struct sockaddr *)&next_hop.addr, sizeof(next_hop.addr));
    return (s == (ssize_t)total_len) ? 0 : -1;
}

int wex_p2p_send_find_node(wex_p2p_node_t *node, const struct sockaddr_in *target_addr, const uint8_t search_id[32]) {
    if (!node || node->udp_fd < 0 || !target_addr || !search_id) return -1;

    uint8_t buffer[sizeof(wex_pkt_hdr_t) + 32];
    wex_pkt_hdr_t *hdr = (wex_pkt_hdr_t *)buffer;
    hdr->magic = htonl(WEX_P2P_MAGIC);
    hdr->type = WEX_PKT_DHT_FIND_NODE;
    memcpy(hdr->sender_pk, node->identity->sign_pk, 32);
    memcpy(hdr->space_id, node->active_space_id, 32);
    hdr->payload_len = htons(32);

    memcpy(buffer + sizeof(wex_pkt_hdr_t), search_id, 32);

    ssize_t s = sendto(node->udp_fd, buffer, sizeof(buffer), 0,
                       (struct sockaddr *)target_addr, sizeof(*target_addr));
    return (s == (ssize_t)sizeof(buffer)) ? 0 : -1;
}

int wex_p2p_punch_hole(wex_p2p_node_t *node, const struct sockaddr_in *rendezvous_addr, const uint8_t target_pk[32]) {
    if (!node || node->udp_fd < 0 || !rendezvous_addr || !target_pk) return -1;

    uint8_t buffer[sizeof(wex_pkt_hdr_t) + 32];
    wex_pkt_hdr_t *hdr = (wex_pkt_hdr_t *)buffer;
    hdr->magic = htonl(WEX_P2P_MAGIC);
    hdr->type = WEX_PKT_NAT_PUNCH_REQ;
    memcpy(hdr->sender_pk, node->identity->sign_pk, 32);
    memcpy(hdr->space_id, node->active_space_id, 32);
    hdr->payload_len = htons(32);
    memcpy(buffer + sizeof(wex_pkt_hdr_t), target_pk, 32);

    ssize_t s = sendto(node->udp_fd, buffer, sizeof(buffer), 0,
                       (struct sockaddr *)rendezvous_addr, sizeof(*rendezvous_addr));
    return (s == (ssize_t)sizeof(buffer)) ? 0 : -1;
}

int wex_p2p_start_node_lookup(wex_p2p_node_t *node, const uint8_t target_id[32]) {
    if (!node || node->udp_fd < 0 || !target_id) return -1;
    if (node->lookup_count >= 4) {
        /* Evict oldest */
        memmove(&node->active_lookups[0], &node->active_lookups[1], 3 * sizeof(wex_dht_lookup_t));
        node->lookup_count = 3;
    }

    wex_dht_lookup_t *lk = &node->active_lookups[node->lookup_count++];
    wex_dht_lookup_init(lk, &node->dht, target_id, get_time_ms());

    wex_dht_contact_t queries[WEX_DHT_ALPHA];
    size_t count = wex_dht_lookup_get_next(lk, WEX_DHT_ALPHA, queries);
    for (size_t i = 0; i < count; i++) {
        wex_p2p_send_find_node(node, &queries[i].addr, target_id);
    }
    return 0;
}

int wex_p2p_announce_space(wex_p2p_node_t *node, const uint8_t space_id[32]) {
    if (!node || node->udp_fd < 0 || !space_id) return -1;

    wex_dht_contact_t closest[WEX_DHT_K];
    size_t found = wex_dht_find_closest(&node->dht, space_id, closest, WEX_DHT_K);

    uint8_t buffer[sizeof(wex_pkt_hdr_t)];
    wex_pkt_hdr_t *hdr = (wex_pkt_hdr_t *)buffer;
    hdr->magic = htonl(WEX_P2P_MAGIC);
    hdr->type = WEX_PKT_DHT_ANNOUNCE;
    memcpy(hdr->sender_pk, node->identity->sign_pk, 32);
    memcpy(hdr->space_id, space_id, 32);
    hdr->payload_len = 0;

    int sent = 0;
    for (size_t i = 0; i < found; i++) {
        ssize_t s = sendto(node->udp_fd, buffer, sizeof(buffer), 0,
                           (struct sockaddr *)&closest[i].addr, sizeof(closest[i].addr));
        if (s == (ssize_t)sizeof(buffer)) sent++;
    }
    return sent;
}

int wex_p2p_find_space_peers(wex_p2p_node_t *node, const uint8_t space_id[32]) {
    if (!node || node->udp_fd < 0 || !space_id) return -1;

    wex_dht_contact_t closest[WEX_DHT_K];
    size_t found = wex_dht_find_closest(&node->dht, space_id, closest, WEX_DHT_K);

    uint8_t buffer[sizeof(wex_pkt_hdr_t)];
    wex_pkt_hdr_t *hdr = (wex_pkt_hdr_t *)buffer;
    hdr->magic = htonl(WEX_P2P_MAGIC);
    hdr->type = WEX_PKT_DHT_GET_PROVIDERS;
    memcpy(hdr->sender_pk, node->identity->sign_pk, 32);
    memcpy(hdr->space_id, space_id, 32);
    hdr->payload_len = 0;

    int sent = 0;
    for (size_t i = 0; i < found; i++) {
        ssize_t s = sendto(node->udp_fd, buffer, sizeof(buffer), 0,
                           (struct sockaddr *)&closest[i].addr, sizeof(closest[i].addr));
        if (s == (ssize_t)sizeof(buffer)) sent++;
    }
    return sent;
}

void wex_p2p_maintain(wex_p2p_node_t *node, uint64_t now_ms) {
    if (!node || node->udp_fd < 0) return;
    if (now_ms - node->last_maintenance_ms < 1000) return;
    node->last_maintenance_ms = now_ms;

    /* Ping active peers periodically to keep NAT mappings open */
    for (size_t i = 0; i < node->peer_count; i++) {
        if (!node->peers[i].active) continue;
        if (now_ms - node->peers[i].last_seen_ms > 60000) {
            /* Peer timed out */
            node->peers[i].active = false;
            if (node->peer_cb) {
                node->peer_cb(&node->peers[i], false, node->user_data);
            }
        } else if (now_ms - node->peers[i].last_seen_ms > 25000) {
            /* Send keepalive ping */
            uint8_t ping_buf[sizeof(wex_pkt_hdr_t)];
            wex_pkt_hdr_t *h = (wex_pkt_hdr_t *)ping_buf;
            h->magic = htonl(WEX_P2P_MAGIC);
            h->type = WEX_PKT_PING;
            memcpy(h->sender_pk, node->identity->sign_pk, 32);
            memcpy(h->space_id, node->active_space_id, 32);
            h->payload_len = 0;
            sendto(node->udp_fd, ping_buf, sizeof(ping_buf), 0,
                   (struct sockaddr *)&node->peers[i].addr, sizeof(node->peers[i].addr));
        }
    }

    /* Prune completed lookups */
    size_t active_idx = 0;
    for (size_t i = 0; i < node->lookup_count; i++) {
        if (!wex_dht_lookup_is_done(&node->active_lookups[i]) && (now_ms - node->active_lookups[i].started_ms < 10000)) {
            if (active_idx != i) {
                node->active_lookups[active_idx] = node->active_lookups[i];
            }
            active_idx++;
        }
    }
    node->lookup_count = active_idx;
}

static void parse_and_deliver_chat_msg(wex_p2p_node_t *node, const uint8_t *payload, size_t payload_len, const uint8_t sender_pk[32], const uint8_t space_id[32]) {
    if (payload_len < 64 + sizeof(uint64_t) + 2) return;

    wex_chat_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    memcpy(msg.space_id, space_id, 32);
    memcpy(msg.author_pk, sender_pk, 32);

    size_t off = 0;

    /* Channel */
    const char *ch = (const char *)(payload + off);
    size_t ch_len = strnlen(ch, payload_len - off) + 1;
    if (ch_len > WEX_NAME_LEN) ch_len = WEX_NAME_LEN;
    strncpy(msg.channel, ch, ch_len - 1);
    off += ch_len;
    if (off >= payload_len) return;

    /* Author */
    const char *auth = (const char *)(payload + off);
    size_t auth_len = strnlen(auth, payload_len - off) + 1;
    if (auth_len > WEX_NAME_LEN) auth_len = WEX_NAME_LEN;
    strncpy(msg.author, auth, auth_len - 1);
    off += auth_len;
    if (off + sizeof(uint64_t) > payload_len) return;

    /* Timestamp */
    memcpy(&msg.timestamp_ms, payload + off, sizeof(uint64_t));
    off += sizeof(uint64_t);
    if (off >= payload_len) return;

    /* Encryption metadata */
    msg.enc_flags = payload[off++];
    if (msg.enc_flags != 0) {
        if (off + 24 + 16 > payload_len) return;
        memcpy(msg.nonce, payload + off, 24);
        off += 24;
        memcpy(msg.mac, payload + off, 16);
        off += 16;
    }

    /* Content */
    const char *content_str = (const char *)(payload + off);
    size_t cont_len = strnlen(content_str, payload_len - off) + 1;
    if (cont_len > WEX_MAX_MSG_LEN) cont_len = WEX_MAX_MSG_LEN;
    strncpy(msg.content, content_str, cont_len - 1);
    off += cont_len;
    if (off + 64 > payload_len) return;

    /* Signature */
    memcpy(msg.signature, payload + off, 64);

    /* Cryptographic verification of author's Ed25519 signature */
    if (wex_chat_msg_verify(&msg)) {
        /* Auto-decrypt if encrypted */
        if (msg.enc_flags == 1) {
            wex_space_t *sp = wex_p2p_get_active_space(node);
            if (sp) {
                wex_chat_msg_decrypt(&msg, sp->space_key);
            }
        } else if (msg.enc_flags == 2) {
            uint8_t peer_x25519[32];
            bool has_key = false;
            for (size_t p = 0; p < node->peer_count; p++) {
                if (memcmp(node->peers[p].pk, msg.author_pk, 32) == 0) {
                    memcpy(peer_x25519, node->peers[p].x25519_pk, 32);
                    has_key = true;
                    break;
                }
            }
            if (!has_key) {
                crypto_eddsa_to_x25519(peer_x25519, msg.author_pk);
            }
            uint8_t secret[32];
            if (wex_ecdh(node->identity->x25519_sk, peer_x25519, secret) == 0) {
                uint8_t dm_key[32];
                if (wex_kdf_session_key(secret, NULL, "WEX_DM_SESSION", dm_key) == 0) {
                    wex_chat_msg_decrypt(&msg, dm_key);
                }
                wex_clean(secret, 32);
                wex_clean(dm_key, 32);
            }
        }

        if (node->journal) {
            wex_journal_append(node->journal, &msg);
        }
        if (node->msg_cb) {
            node->msg_cb(&msg, node->user_data);
        }
    } else {
        fprintf(stderr, "[!] Warning: Dropped forged/corrupted P2P message from %s\n", msg.author);
    }
}

int wex_p2p_send_dm(wex_p2p_node_t *node, const uint8_t target_pk[32], const char *text) {
    if (!node || !target_pk || !text) return -1;

    uint8_t target_x25519[32];
    bool found_peer = false;
    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(target_addr));

    for (size_t i = 0; i < node->peer_count; i++) {
        if (node->peers[i].active && memcmp(node->peers[i].pk, target_pk, 32) == 0) {
            memcpy(target_x25519, node->peers[i].x25519_pk, 32);
            target_addr = node->peers[i].addr;
            found_peer = true;
            break;
        }
    }

    if (!found_peer) {
        crypto_eddsa_to_x25519(target_x25519, target_pk);
    }

    uint8_t secret[32];
    if (wex_ecdh(node->identity->x25519_sk, target_x25519, secret) != 0) {
        return -1;
    }

    uint8_t dm_key[32];
    if (wex_kdf_session_key(secret, NULL, "WEX_DM_SESSION", dm_key) != 0) {
        wex_clean(secret, 32);
        return -1;
    }
    wex_clean(secret, 32);

    wex_chat_msg_t dm;
    memset(&dm, 0, sizeof(dm));
    /* Channel label for DM is dm:<hex_prefix> */
    char target_hex[65];
    wex_id_to_hex(target_pk, target_hex);
    snprintf(dm.channel, sizeof(dm.channel), "dm:%.58s", target_hex);
    strncpy(dm.content, text, sizeof(dm.content) - 1);

    if (wex_chat_msg_encrypt(&dm, dm_key, 2) != 0) {
        wex_clean(dm_key, 32);
        return -1;
    }
    wex_clean(dm_key, 32);

    if (wex_chat_msg_sign(&dm, node->identity) != 0) {
        return -1;
    }

    if (found_peer) {
        uint8_t buf[WEX_MAX_UDP_PAYLOAD];
        size_t len = serialize_chat_msg(&dm, buf, sizeof(buf));
        if (len > 0) {
            sendto(node->udp_fd, buf, len, 0, (struct sockaddr *)&target_addr, sizeof(target_addr));
        }
    } else {
        wex_p2p_relay_msg(node, target_pk, &dm);
    }

    if (node->journal) {
        wex_chat_msg_t plain_copy = dm;
        plain_copy.enc_flags = 0;
        strncpy(plain_copy.content, text, sizeof(plain_copy.content) - 1);
        wex_journal_append(node->journal, &plain_copy);
    }

    return 0;
}

int wex_p2p_poll(wex_p2p_node_t *node, int timeout_ms) {
    if (!node || node->udp_fd < 0) return -1;

    wex_p2p_maintain(node, get_time_ms());

    struct pollfd pfd;
    pfd.fd = node->udp_fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return ret;

    if (!(pfd.revents & POLLIN)) return 0;

    uint8_t buffer[WEX_MAX_UDP_PAYLOAD];
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);

    ssize_t n = recvfrom(node->udp_fd, buffer, sizeof(buffer), 0,
                         (struct sockaddr *)&src_addr, &addr_len);
    if (n < (ssize_t)sizeof(wex_pkt_hdr_t)) return 0;

    /* Check magic */
    uint32_t magic = ntohl(*(uint32_t *)buffer);
    if (magic != WEX_P2P_MAGIC) {
        return 0;
    }

    uint8_t pkt_type = buffer[4];

    /* Handle Overlay Relay */
    if (pkt_type == WEX_PKT_OVERLAY_RELAY) {
        if (n < (ssize_t)sizeof(wex_dht_relay_hdr_t)) return 0;
        wex_dht_relay_hdr_t *relay_hdr = (wex_dht_relay_hdr_t *)buffer;
        uint16_t inner_len = ntohs(relay_hdr->payload_len);
        if ((size_t)n < sizeof(wex_dht_relay_hdr_t) + inner_len) return 0;

        /* Update DHT contact for the intermediate sender */
        wex_dht_update_contact(&node->dht, relay_hdr->src_id, &src_addr, get_time_ms());

        /* Check if we are destination */
        if (memcmp(relay_hdr->dst_id, node->identity->sign_pk, 32) == 0) {
            const wex_pkt_hdr_t *inner_hdr = (const wex_pkt_hdr_t *)(buffer + sizeof(wex_dht_relay_hdr_t));
            uint16_t msg_payload_len = ntohs(inner_hdr->payload_len);
            const uint8_t *msg_payload = (const uint8_t *)inner_hdr + sizeof(wex_pkt_hdr_t);
            parse_and_deliver_chat_msg(node, msg_payload, msg_payload_len, inner_hdr->sender_pk, inner_hdr->space_id);
        } else if (relay_hdr->hops_left > 1) {
            /* Forward transitively */
            relay_hdr->hops_left--;
            wex_dht_contact_t next_hop;
            if (wex_dht_next_hop(&node->dht, relay_hdr->dst_id, &next_hop)) {
                sendto(node->udp_fd, buffer, (size_t)n, 0,
                       (struct sockaddr *)&next_hop.addr, sizeof(next_hop.addr));
            }
        }
        return 1;
    }

    const wex_pkt_hdr_t *hdr = (const wex_pkt_hdr_t *)buffer;
    uint16_t payload_len = ntohs(hdr->payload_len);
    if ((size_t)n < sizeof(wex_pkt_hdr_t) + payload_len) {
        return 0;
    }

    const uint8_t *payload = buffer + sizeof(wex_pkt_hdr_t);

    /* Update Kademlia DHT routing table and topic providers */
    wex_dht_update_contact(&node->dht, hdr->sender_pk, &src_addr, get_time_ms());
    wex_dht_announce_topic(&node->dht, hdr->space_id, hdr->sender_pk, &src_addr, 3600000ULL);

    switch (hdr->type) {
        case WEX_PKT_HELLO: {
            char peer_nick[WEX_NAME_LEN];
            memset(peer_nick, 0, sizeof(peer_nick));
            wex_pkt_hello_t hello;
            const uint8_t *xpk = NULL;
            if (payload_len >= sizeof(wex_pkt_hello_t)) {
                memcpy(&hello, payload, sizeof(hello));
                xpk = hello.x25519_pk;
                strncpy(peer_nick, hello.nickname, sizeof(peer_nick) - 1);
            } else if (payload_len > 0) {
                size_t cpy = (payload_len < sizeof(peer_nick) - 1) ? payload_len : sizeof(peer_nick) - 1;
                memcpy(peer_nick, payload, cpy);
            }
            wex_peer_t *p = find_or_add_peer(node, &src_addr, hdr->sender_pk, peer_nick, xpk);
            if (p && node->peer_cb) {
                node->peer_cb(p, true, node->user_data);
            }

            /* Send WELCOME back */
            uint8_t resp_buf[sizeof(wex_pkt_hdr_t) + sizeof(wex_pkt_hello_t)];
            wex_pkt_hdr_t *resp_hdr = (wex_pkt_hdr_t *)resp_buf;
            resp_hdr->magic = htonl(WEX_P2P_MAGIC);
            resp_hdr->type = WEX_PKT_WELCOME;
            memcpy(resp_hdr->sender_pk, node->identity->sign_pk, 32);
            memcpy(resp_hdr->space_id, node->active_space_id, 32);

            wex_pkt_hello_t welcome;
            memset(&welcome, 0, sizeof(welcome));
            memcpy(welcome.x25519_pk, node->identity->x25519_pk, 32);
            strncpy(welcome.nickname, node->identity->nickname, sizeof(welcome.nickname) - 1);

            resp_hdr->payload_len = htons((uint16_t)sizeof(welcome));
            memcpy(resp_buf + sizeof(wex_pkt_hdr_t), &welcome, sizeof(welcome));

            sendto(node->udp_fd, resp_buf, sizeof(resp_buf), 0,
                   (struct sockaddr *)&src_addr, sizeof(src_addr));
            wex_p2p_send_space_info(node, &src_addr);
            break;
        }

        case WEX_PKT_WELCOME: {
            char peer_nick[WEX_NAME_LEN];
            memset(peer_nick, 0, sizeof(peer_nick));
            wex_pkt_hello_t hello;
            const uint8_t *xpk = NULL;
            if (payload_len >= sizeof(wex_pkt_hello_t)) {
                memcpy(&hello, payload, sizeof(hello));
                xpk = hello.x25519_pk;
                strncpy(peer_nick, hello.nickname, sizeof(peer_nick) - 1);
            } else if (payload_len > 0) {
                size_t cpy = (payload_len < sizeof(peer_nick) - 1) ? payload_len : sizeof(peer_nick) - 1;
                memcpy(peer_nick, payload, cpy);
            }
            wex_peer_t *p = find_or_add_peer(node, &src_addr, hdr->sender_pk, peer_nick, xpk);
            if (p) {
                if (node->peer_cb) {
                    node->peer_cb(p, true, node->user_data);
                }
                if (!p->synced) {
                    wex_p2p_request_sync(node, &p->addr);
                    p->synced = true;
                }
                wex_p2p_send_space_info(node, &src_addr);
            }
            break;
        }

        case WEX_PKT_SPACE_INFO: {
            if (payload_len < 2) break;
            size_t off = 0;

            const char *sp_name = (const char *)(payload + off);
            size_t nlen = strnlen(sp_name, payload_len - off) + 1;
            off += nlen;
            if (off >= payload_len) break;

            uint8_t ch_count = payload[off++];

            wex_space_t space;
            memset(&space, 0, sizeof(space));
            memcpy(space.space_id, hdr->space_id, 32);
            strncpy(space.name, sp_name, sizeof(space.name) - 1);
            memcpy(space.owner_pk, hdr->sender_pk, 32);

            for (uint8_t c = 0; c < ch_count && off < payload_len; c++) {
                const char *chn = (const char *)(payload + off);
                size_t clen = strnlen(chn, payload_len - off) + 1;
                off += clen;
                if (space.channel_count < WEX_MAX_CHANNELS) {
                    strncpy(space.channels[space.channel_count].name, chn, WEX_NAME_LEN - 1);
                    space.channel_count++;
                }
            }

            wex_p2p_add_space(node, &space);
            break;
        }

        case WEX_PKT_SYNC_REQ: {
            char target_ch[WEX_NAME_LEN] = "general";
            if (payload_len > 0) {
                size_t cpy = (payload_len < sizeof(target_ch) - 1) ? payload_len : sizeof(target_ch) - 1;
                memcpy(target_ch, payload, cpy);
                target_ch[cpy] = '\0';
            }

            if (node->sync_cb) {
                wex_chat_msg_t hist_msgs[32];
                size_t hist_count = node->sync_cb(hdr->space_id, target_ch, hist_msgs, 32, node->user_data);
                for (size_t i = 0; i < hist_count; i++) {
                    uint8_t out_buf[WEX_MAX_UDP_PAYLOAD];
                    size_t out_len = serialize_chat_msg(&hist_msgs[i], out_buf, sizeof(out_buf));
                    if (out_len > 0) {
                        sendto(node->udp_fd, out_buf, out_len, 0,
                               (struct sockaddr *)&src_addr, sizeof(src_addr));
                    }
                }
            } else if (node->journal) {
                wex_chat_msg_t hist_msgs[32];
                size_t hist_count = wex_journal_read_recent(node->journal, hdr->space_id, target_ch, hist_msgs, 32);
                for (size_t i = 0; i < hist_count; i++) {
                    uint8_t out_buf[WEX_MAX_UDP_PAYLOAD];
                    size_t out_len = serialize_chat_msg(&hist_msgs[i], out_buf, sizeof(out_buf));
                    if (out_len > 0) {
                        sendto(node->udp_fd, out_buf, out_len, 0,
                               (struct sockaddr *)&src_addr, sizeof(src_addr));
                    }
                }
            }
            break;
        }

        case WEX_PKT_PING: {
            uint8_t pong_buf[sizeof(wex_pkt_hdr_t) + sizeof(struct sockaddr_in)];
            wex_pkt_hdr_t *pong_hdr = (wex_pkt_hdr_t *)pong_buf;
            pong_hdr->magic = htonl(WEX_P2P_MAGIC);
            pong_hdr->type = WEX_PKT_PONG;
            memcpy(pong_hdr->sender_pk, node->identity->sign_pk, 32);
            memcpy(pong_hdr->space_id, hdr->space_id, 32);
            pong_hdr->payload_len = htons((uint16_t)sizeof(struct sockaddr_in));
            memcpy(pong_buf + sizeof(wex_pkt_hdr_t), &src_addr, sizeof(struct sockaddr_in));
            sendto(node->udp_fd, pong_buf, sizeof(pong_buf), 0,
                   (struct sockaddr *)&src_addr, sizeof(src_addr));
            break;
        }

        case WEX_PKT_PONG: {
            if (payload_len >= sizeof(struct sockaddr_in)) {
                if (!node->has_external_addr) {
                    memcpy(&node->external_addr, payload, sizeof(struct sockaddr_in));
                    node->has_external_addr = true;
                }
            }
            break;
        }

        case WEX_PKT_NAT_PUNCH_REQ: {
            if (payload_len < 32) break;
            const uint8_t *target_pk = payload;

            const struct sockaddr_in *target_addr = NULL;
            for (size_t i = 0; i < node->peer_count; i++) {
                if (node->peers[i].active && memcmp(node->peers[i].pk, target_pk, 32) == 0) {
                    target_addr = &node->peers[i].addr;
                    break;
                }
            }

            if (target_addr) {
                /* Trigger target to punch to caller */
                uint8_t trig_to_b[sizeof(wex_pkt_hdr_t) + 32 + sizeof(struct sockaddr_in)];
                wex_pkt_hdr_t *h_b = (wex_pkt_hdr_t *)trig_to_b;
                h_b->magic = htonl(WEX_P2P_MAGIC);
                h_b->type = WEX_PKT_NAT_PUNCH_TRIGGER;
                memcpy(h_b->sender_pk, node->identity->sign_pk, 32);
                memcpy(h_b->space_id, hdr->space_id, 32);
                h_b->payload_len = htons(32 + sizeof(struct sockaddr_in));
                memcpy(trig_to_b + sizeof(wex_pkt_hdr_t), hdr->sender_pk, 32);
                memcpy(trig_to_b + sizeof(wex_pkt_hdr_t) + 32, &src_addr, sizeof(struct sockaddr_in));
                sendto(node->udp_fd, trig_to_b, sizeof(trig_to_b), 0,
                       (struct sockaddr *)target_addr, sizeof(*target_addr));

                /* Trigger caller to punch to target */
                uint8_t trig_to_a[sizeof(wex_pkt_hdr_t) + 32 + sizeof(struct sockaddr_in)];
                wex_pkt_hdr_t *h_a = (wex_pkt_hdr_t *)trig_to_a;
                h_a->magic = htonl(WEX_P2P_MAGIC);
                h_a->type = WEX_PKT_NAT_PUNCH_TRIGGER;
                memcpy(h_a->sender_pk, node->identity->sign_pk, 32);
                memcpy(h_a->space_id, hdr->space_id, 32);
                h_a->payload_len = htons(32 + sizeof(struct sockaddr_in));
                memcpy(trig_to_a + sizeof(wex_pkt_hdr_t), target_pk, 32);
                memcpy(trig_to_a + sizeof(wex_pkt_hdr_t) + 32, target_addr, sizeof(struct sockaddr_in));
                sendto(node->udp_fd, trig_to_a, sizeof(trig_to_a), 0,
                       (struct sockaddr *)&src_addr, sizeof(src_addr));
            }
            break;
        }

        case WEX_PKT_NAT_PUNCH_TRIGGER: {
            if (payload_len < 32 + sizeof(struct sockaddr_in)) break;
            const uint8_t *remote_pk = payload;
            struct sockaddr_in remote_addr;
            memcpy(&remote_addr, payload + 32, sizeof(struct sockaddr_in));

            /* Send direct UDP punch probes */
            uint8_t punch_pkt[sizeof(wex_pkt_hdr_t)];
            wex_pkt_hdr_t *p_hdr = (wex_pkt_hdr_t *)punch_pkt;
            p_hdr->magic = htonl(WEX_P2P_MAGIC);
            p_hdr->type = WEX_PKT_NAT_PUNCH;
            memcpy(p_hdr->sender_pk, node->identity->sign_pk, 32);
            memcpy(p_hdr->space_id, hdr->space_id, 32);
            p_hdr->payload_len = 0;

            for (int k = 0; k < 3; k++) {
                sendto(node->udp_fd, punch_pkt, sizeof(punch_pkt), 0,
                       (struct sockaddr *)&remote_addr, sizeof(remote_addr));
            }

            /* Send HELLO to finalize connection */
            find_or_add_peer(node, &remote_addr, remote_pk, "Peer", NULL);
            uint8_t hello_buf[sizeof(wex_pkt_hdr_t) + sizeof(wex_pkt_hello_t)];
            wex_pkt_hdr_t *h_hdr = (wex_pkt_hdr_t *)hello_buf;
            h_hdr->magic = htonl(WEX_P2P_MAGIC);
            h_hdr->type = WEX_PKT_HELLO;
            memcpy(h_hdr->sender_pk, node->identity->sign_pk, 32);
            memcpy(h_hdr->space_id, node->active_space_id, 32);
            wex_pkt_hello_t h_payload;
            memset(&h_payload, 0, sizeof(h_payload));
            memcpy(h_payload.x25519_pk, node->identity->x25519_pk, 32);
            strncpy(h_payload.nickname, node->identity->nickname, sizeof(h_payload.nickname) - 1);
            h_hdr->payload_len = htons((uint16_t)sizeof(h_payload));
            memcpy(hello_buf + sizeof(wex_pkt_hdr_t), &h_payload, sizeof(h_payload));
            sendto(node->udp_fd, hello_buf, sizeof(hello_buf), 0,
                   (struct sockaddr *)&remote_addr, sizeof(remote_addr));
            break;
        }

        case WEX_PKT_NAT_PUNCH: {
            uint8_t hello_buf[512];
            wex_pkt_hdr_t *h_hdr = (wex_pkt_hdr_t *)hello_buf;
            h_hdr->magic = htonl(WEX_P2P_MAGIC);
            h_hdr->type = WEX_PKT_HELLO;
            memcpy(h_hdr->sender_pk, node->identity->sign_pk, 32);
            memcpy(h_hdr->space_id, node->active_space_id, 32);
            size_t nlen = strlen(node->identity->nickname) + 1;
            h_hdr->payload_len = htons((uint16_t)nlen);
            memcpy(hello_buf + sizeof(wex_pkt_hdr_t), node->identity->nickname, nlen);
            sendto(node->udp_fd, hello_buf, sizeof(wex_pkt_hdr_t) + nlen, 0,
                   (struct sockaddr *)&src_addr, sizeof(src_addr));
            break;
        }

        case WEX_PKT_DHT_FIND_NODE: {
            if (payload_len < 32) break;
            const uint8_t *search_target = payload;

            /* Query Kademlia routing table for K closest contacts */
            wex_dht_contact_t closest[WEX_DHT_K];
            size_t found = wex_dht_find_closest(&node->dht, search_target, closest, WEX_DHT_K);

            /* Serialize and send WEX_PKT_DHT_NODES response */
            uint8_t resp_buf[WEX_MAX_UDP_PAYLOAD];
            wex_pkt_hdr_t *resp_hdr = (wex_pkt_hdr_t *)resp_buf;
            resp_hdr->magic = htonl(WEX_P2P_MAGIC);
            resp_hdr->type = WEX_PKT_DHT_NODES;
            memcpy(resp_hdr->sender_pk, node->identity->sign_pk, 32);
            memcpy(resp_hdr->space_id, hdr->space_id, 32);

            uint8_t *out = resp_buf + sizeof(wex_pkt_hdr_t);
            out[0] = (uint8_t)found;
            size_t off = 1;

            for (size_t i = 0; i < found; i++) {
                memcpy(out + off, closest[i].id, 32);
                off += 32;
                memcpy(out + off, &closest[i].addr, sizeof(struct sockaddr_in));
                off += sizeof(struct sockaddr_in);
            }

            resp_hdr->payload_len = htons((uint16_t)off);
            sendto(node->udp_fd, resp_buf, sizeof(wex_pkt_hdr_t) + off, 0,
                   (struct sockaddr *)&src_addr, sizeof(src_addr));
            break;
        }

        case WEX_PKT_DHT_NODES: {
            if (payload_len < 1) break;
            uint8_t count = payload[0];
            size_t item_size = 32 + sizeof(struct sockaddr_in);
            if (payload_len < 1 + count * item_size) break;

            size_t off = 1;
            wex_dht_contact_t disc_contacts[WEX_DHT_K];
            size_t disc_count = 0;

            for (uint8_t i = 0; i < count; i++) {
                const uint8_t *peer_id = payload + off;
                off += 32;
                struct sockaddr_in peer_addr;
                memcpy(&peer_addr, payload + off, sizeof(struct sockaddr_in));
                off += sizeof(struct sockaddr_in);

                /* Add/update discovered node in our Kademlia routing table */
                wex_dht_update_contact(&node->dht, peer_id, &peer_addr, get_time_ms());

                if (disc_count < WEX_DHT_K) {
                    memcpy(disc_contacts[disc_count].id, peer_id, 32);
                    disc_contacts[disc_count].addr = peer_addr;
                    disc_contacts[disc_count].last_seen_ms = get_time_ms();
                    disc_count++;
                }
            }

            /* Advance active iterative lookups */
            for (size_t l = 0; l < node->lookup_count; l++) {
                wex_dht_lookup_t *lk = &node->active_lookups[l];
                if (!wex_dht_lookup_is_done(lk)) {
                    wex_dht_lookup_add_candidates(lk, disc_contacts, disc_count);
                    wex_dht_contact_t next_queries[WEX_DHT_ALPHA];
                    size_t nq = wex_dht_lookup_get_next(lk, WEX_DHT_ALPHA, next_queries);
                    for (size_t q = 0; q < nq; q++) {
                        wex_p2p_send_find_node(node, &next_queries[q].addr, lk->target_id);
                    }
                }
            }
            break;
        }

        case WEX_PKT_DHT_ANNOUNCE: {
            wex_dht_announce_topic(&node->dht, hdr->space_id, hdr->sender_pk, &src_addr, 3600000ULL);
            break;
        }

        case WEX_PKT_DHT_GET_PROVIDERS: {
            wex_dht_provider_t provs[16];
            size_t pcount = wex_dht_get_topic_providers(&node->dht, hdr->space_id, provs, 16);

            uint8_t resp_buf[WEX_MAX_UDP_PAYLOAD];
            wex_pkt_hdr_t *r_hdr = (wex_pkt_hdr_t *)resp_buf;
            r_hdr->magic = htonl(WEX_P2P_MAGIC);
            r_hdr->type = WEX_PKT_DHT_PROVIDERS;
            memcpy(r_hdr->sender_pk, node->identity->sign_pk, 32);
            memcpy(r_hdr->space_id, hdr->space_id, 32);

            uint8_t *out = resp_buf + sizeof(wex_pkt_hdr_t);
            out[0] = (uint8_t)pcount;
            size_t off = 1;
            for (size_t i = 0; i < pcount; i++) {
                memcpy(out + off, provs[i].peer_id, 32);
                off += 32;
                memcpy(out + off, &provs[i].addr, sizeof(struct sockaddr_in));
                off += sizeof(struct sockaddr_in);
            }
            r_hdr->payload_len = htons((uint16_t)off);
            sendto(node->udp_fd, resp_buf, sizeof(wex_pkt_hdr_t) + off, 0,
                   (struct sockaddr *)&src_addr, sizeof(src_addr));
            break;
        }

        case WEX_PKT_DHT_PROVIDERS: {
            if (payload_len < 1) break;
            uint8_t pcount = payload[0];
            size_t item_sz = 32 + sizeof(struct sockaddr_in);
            if (payload_len < 1 + pcount * item_sz) break;

            size_t off = 1;
            for (uint8_t i = 0; i < pcount; i++) {
                const uint8_t *peer_id = payload + off;
                off += 32;
                struct sockaddr_in peer_addr;
                memcpy(&peer_addr, payload + off, sizeof(struct sockaddr_in));
                off += sizeof(struct sockaddr_in);

                if (memcmp(peer_id, node->identity->sign_pk, 32) != 0) {
                    char ip_buf[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &peer_addr.sin_addr, ip_buf, sizeof(ip_buf));
                    wex_p2p_connect(node, ip_buf, ntohs(peer_addr.sin_port));
                }
            }
            break;
        }

        case WEX_PKT_CHAT_MSG: {
            parse_and_deliver_chat_msg(node, payload, payload_len, hdr->sender_pk, hdr->space_id);
            break;
        }

        case WEX_PKT_BLOB_OFFER: {
            if (payload_len < sizeof(wex_blob_info_t)) break;
            wex_blob_info_t binfo;
            memcpy(&binfo, payload, sizeof(wex_blob_info_t));

            if (node->blob_cb) {
                node->blob_cb(&binfo, NULL, node->user_data);
            }
            break;
        }

        case WEX_PKT_BLOB_REQ: {
            if (payload_len < 36) break;
            uint8_t req_hash[32];
            memcpy(req_hash, payload, 32);
            uint32_t net_idx = 0;
            memcpy(&net_idx, payload + 32, sizeof(net_idx));
            uint32_t chunk_idx = ntohl(net_idx);

            uint8_t *blob_data = NULL;
            size_t blob_sz = 0;
            if (wex_blob_load(node->data_dir, req_hash, &blob_data, &blob_sz) == 0) {
                uint32_t total_chunks = (blob_sz == 0) ? 1 : (uint32_t)((blob_sz + WEX_BLOB_CHUNK_SIZE - 1) / WEX_BLOB_CHUNK_SIZE);
                size_t offset = (size_t)chunk_idx * WEX_BLOB_CHUNK_SIZE;
                if (offset <= blob_sz) {
                    size_t rem = blob_sz - offset;
                    size_t chunk_len = (rem > WEX_BLOB_CHUNK_SIZE) ? WEX_BLOB_CHUNK_SIZE : rem;
                    wex_p2p_send_blob_chunk(node, &src_addr, req_hash, chunk_idx, total_chunks, blob_data + offset, chunk_len);
                }
                free(blob_data);
            }
            break;
        }

        case WEX_PKT_BLOB_CHUNK: {
            if (payload_len < sizeof(wex_blob_chunk_hdr_t)) break;
            wex_blob_chunk_hdr_t chdr;
            memcpy(&chdr, payload, sizeof(wex_blob_chunk_hdr_t));
            uint32_t c_idx = ntohl(chdr.chunk_idx);
            uint16_t d_len = ntohs(chdr.data_len);
            if (payload_len < sizeof(wex_blob_chunk_hdr_t) + d_len) break;

            const uint8_t *c_data = payload + sizeof(wex_blob_chunk_hdr_t);

            if (node->is_downloading && memcmp(chdr.root_hash, node->active_download.info.root_hash, 32) == 0) {
                bool complete = wex_blob_assembler_put_chunk(&node->active_download, c_idx, c_data, d_len);
                if (complete) {
                    node->is_downloading = false;
                    wex_blob_save(node->data_dir, &node->active_download.info, node->active_download.data);
                    if (node->blob_cb) {
                        node->blob_cb(&node->active_download.info, node->active_download.data, node->user_data);
                    }
                    wex_blob_assembler_free(&node->active_download);
                } else {
                    /* Request next missing chunk */
                    for (uint32_t i = 0; i < node->active_download.info.total_chunks; i++) {
                        if (!node->active_download.chunk_received[i]) {
                            wex_p2p_request_blob_chunk(node, &src_addr, chdr.root_hash, i);
                            break;
                        }
                    }
                }
            }
            break;
        }

        case WEX_PKT_MSG_ACTION: {
            if (payload_len < sizeof(wex_msg_action_t)) break;
            wex_msg_action_t action;
            memcpy(&action, payload, sizeof(wex_msg_action_t));
            if (!wex_msg_action_verify(&action)) {
                break;
            }
            if (node->action_cb) {
                node->action_cb(&action, node->user_data);
            }
            break;
        }

        case WEX_PKT_VOICE_FRAME: {
            if (payload_len < sizeof(char) * WEX_NAME_LEN + sizeof(wex_voice_hdr_t)) break;
            char v_channel[WEX_NAME_LEN];
            memcpy(v_channel, payload, WEX_NAME_LEN);
            v_channel[WEX_NAME_LEN - 1] = '\0';

            wex_voice_hdr_t v_hdr;
            memcpy(&v_hdr, payload + WEX_NAME_LEN, sizeof(wex_voice_hdr_t));

            size_t audio_offset = WEX_NAME_LEN + sizeof(wex_voice_hdr_t);
            size_t audio_sz = (payload_len > audio_offset) ? (payload_len - audio_offset) : 0;
            const uint8_t *audio_ptr = (audio_sz > 0) ? (payload + audio_offset) : NULL;

            if (node->voice_cb) {
                node->voice_cb(v_channel, &v_hdr, audio_ptr, audio_sz, node->user_data);
            }
            break;
        }

        default:
            break;
    }

    return 1;
}

void wex_p2p_close(wex_p2p_node_t *node) {
    if (!node) return;
    if (node->is_downloading) {
        wex_blob_assembler_free(&node->active_download);
        node->is_downloading = false;
    }
    if (node->udp_fd >= 0) {
        close(node->udp_fd);
        node->udp_fd = -1;
    }
    node->running = false;
    node->peer_count = 0;
}
