#define _GNU_SOURCE
#include "wex_p2p.h"
#include "wex_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
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

static wex_peer_t *find_or_add_peer(wex_p2p_node_t *node, const struct sockaddr_in *addr, const uint8_t pk[32], const char *nickname) {
    for (size_t i = 0; i < node->peer_count; i++) {
        if (memcmp(node->peers[i].pk, pk, 32) == 0 ||
            (node->peers[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
             node->peers[i].addr.sin_port == addr->sin_port)) {
            node->peers[i].addr = *addr;
            memcpy(node->peers[i].pk, pk, 32);
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

int wex_p2p_connect(wex_p2p_node_t *node, const char *ip_str, int port) {
    if (!node || node->udp_fd < 0 || !ip_str) return -1;

    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip_str, &target_addr.sin_addr) <= 0) {
        return -1;
    }

    /* Send WEX_PKT_HELLO */
    uint8_t buffer[512];
    wex_pkt_hdr_t *hdr = (wex_pkt_hdr_t *)buffer;
    hdr->magic = htonl(WEX_P2P_MAGIC);
    hdr->type = WEX_PKT_HELLO;
    memcpy(hdr->sender_pk, node->identity->sign_pk, 32);
    memcpy(hdr->space_id, node->active_space_id, 32);

    size_t nick_len = strlen(node->identity->nickname) + 1;
    hdr->payload_len = htons((uint16_t)nick_len);
    memcpy(buffer + sizeof(wex_pkt_hdr_t), node->identity->nickname, nick_len);

    size_t total_len = sizeof(wex_pkt_hdr_t) + nick_len;
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

int wex_p2p_poll(wex_p2p_node_t *node, int timeout_ms) {
    if (!node || node->udp_fd < 0) return -1;

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

    const wex_pkt_hdr_t *hdr = (const wex_pkt_hdr_t *)buffer;
    if (ntohl(hdr->magic) != WEX_P2P_MAGIC) {
        return 0;
    }

    uint16_t payload_len = ntohs(hdr->payload_len);
    if ((size_t)n < sizeof(wex_pkt_hdr_t) + payload_len) {
        return 0;
    }

    const uint8_t *payload = buffer + sizeof(wex_pkt_hdr_t);

    switch (hdr->type) {
        case WEX_PKT_HELLO: {
            char peer_nick[WEX_NAME_LEN];
            memset(peer_nick, 0, sizeof(peer_nick));
            if (payload_len > 0) {
                size_t cpy = (payload_len < sizeof(peer_nick) - 1) ? payload_len : sizeof(peer_nick) - 1;
                memcpy(peer_nick, payload, cpy);
            }
            wex_peer_t *p = find_or_add_peer(node, &src_addr, hdr->sender_pk, peer_nick);
            if (p && node->peer_cb) {
                node->peer_cb(p, true, node->user_data);
            }

            /* Send WELCOME back */
            uint8_t resp_buf[512];
            wex_pkt_hdr_t *resp_hdr = (wex_pkt_hdr_t *)resp_buf;
            resp_hdr->magic = htonl(WEX_P2P_MAGIC);
            resp_hdr->type = WEX_PKT_WELCOME;
            memcpy(resp_hdr->sender_pk, node->identity->sign_pk, 32);
            memcpy(resp_hdr->space_id, node->active_space_id, 32);

            size_t my_nick_len = strlen(node->identity->nickname) + 1;
            resp_hdr->payload_len = htons((uint16_t)my_nick_len);
            memcpy(resp_buf + sizeof(wex_pkt_hdr_t), node->identity->nickname, my_nick_len);

            sendto(node->udp_fd, resp_buf, sizeof(wex_pkt_hdr_t) + my_nick_len, 0,
                   (struct sockaddr *)&src_addr, sizeof(src_addr));
            break;
        }

        case WEX_PKT_WELCOME: {
            char peer_nick[WEX_NAME_LEN];
            memset(peer_nick, 0, sizeof(peer_nick));
            if (payload_len > 0) {
                size_t cpy = (payload_len < sizeof(peer_nick) - 1) ? payload_len : sizeof(peer_nick) - 1;
                memcpy(peer_nick, payload, cpy);
            }
            wex_peer_t *p = find_or_add_peer(node, &src_addr, hdr->sender_pk, peer_nick);
            if (p) {
                if (node->peer_cb) {
                    node->peer_cb(p, true, node->user_data);
                }
                /* Immediately request swarm history synchronization! */
                if (!p->synced) {
                    wex_p2p_request_sync(node, &p->addr);
                    p->synced = true;
                }
            }
            break;
        }

        case WEX_PKT_SYNC_REQ: {
            /* Peer is asking for recent chat history */
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
            }
            break;
        }

        case WEX_PKT_PING: {
            uint8_t pong_buf[sizeof(wex_pkt_hdr_t)];
            wex_pkt_hdr_t *pong_hdr = (wex_pkt_hdr_t *)pong_buf;
            pong_hdr->magic = htonl(WEX_P2P_MAGIC);
            pong_hdr->type = WEX_PKT_PONG;
            memcpy(pong_hdr->sender_pk, node->identity->sign_pk, 32);
            memcpy(pong_hdr->space_id, hdr->space_id, 32);
            pong_hdr->payload_len = 0;
            sendto(node->udp_fd, pong_buf, sizeof(pong_buf), 0,
                   (struct sockaddr *)&src_addr, sizeof(src_addr));
            break;
        }

        case WEX_PKT_CHAT_MSG: {
            if (payload_len < 64 + sizeof(uint64_t) + 2) return 0;

            wex_chat_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            memcpy(msg.space_id, hdr->space_id, 32);
            memcpy(msg.author_pk, hdr->sender_pk, 32);

            size_t off = 0;

            /* Channel */
            const char *ch = (const char *)(payload + off);
            size_t ch_len = strnlen(ch, payload_len - off) + 1;
            if (ch_len > WEX_NAME_LEN) ch_len = WEX_NAME_LEN;
            strncpy(msg.channel, ch, ch_len - 1);
            off += ch_len;
            if (off >= payload_len) return 0;

            /* Author */
            const char *auth = (const char *)(payload + off);
            size_t auth_len = strnlen(auth, payload_len - off) + 1;
            if (auth_len > WEX_NAME_LEN) auth_len = WEX_NAME_LEN;
            strncpy(msg.author, auth, auth_len - 1);
            off += auth_len;
            if (off + sizeof(uint64_t) > payload_len) return 0;

            /* Timestamp */
            memcpy(&msg.timestamp_ms, payload + off, sizeof(uint64_t));
            off += sizeof(uint64_t);
            if (off >= payload_len) return 0;

            /* Content */
            const char *content_str = (const char *)(payload + off);
            size_t cont_len = strnlen(content_str, payload_len - off) + 1;
            if (cont_len > WEX_MAX_MSG_LEN) cont_len = WEX_MAX_MSG_LEN;
            strncpy(msg.content, content_str, cont_len - 1);
            off += cont_len;
            if (off + 64 > payload_len) return 0;

            /* Signature */
            memcpy(msg.signature, payload + off, 64);

            /* Cryptographic verification of author's Ed25519 signature */
            if (wex_chat_msg_verify(&msg)) {
                if (node->msg_cb) {
                    node->msg_cb(&msg, node->user_data);
                }
            } else {
                fprintf(stderr, "[!] Warning: Dropped forged/corrupted P2P message from %s\n", msg.author);
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
    if (node->udp_fd >= 0) {
        close(node->udp_fd);
        node->udp_fd = -1;
    }
    node->running = false;
    node->peer_count = 0;
}
