#define _GNU_SOURCE
#include "wex_nat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>

#define STUN_MAGIC 0x2112A442
#define STUN_ATTR_MAPPED_ADDR     0x0001
#define STUN_ATTR_XOR_MAPPED_ADDR 0x0020

static void get_random_bytes(void *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, buf, len);
        (void)r;
        close(fd);
    } else {
        /* Fallback */
        uint8_t *p = (uint8_t *)buf;
        for (size_t i = 0; i < len; i++) {
            p[i] = (uint8_t)(rand() & 0xFF);
        }
    }
}

void wex_addr_to_string(const struct sockaddr_in *addr, char *out_str, size_t max_len) {
    if (!addr || !out_str || max_len == 0) return;
    char ip_buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr->sin_addr, ip_buf, sizeof(ip_buf));
    snprintf(out_str, max_len, "%s:%d", ip_buf, ntohs(addr->sin_port));
}

void wex_detect_local_lan_ip(char *out_ip, size_t max_len) {
    if (!out_ip || max_len == 0) return;
    strncpy(out_ip, "127.0.0.1", max_len - 1);
    out_ip[max_len - 1] = '\0';

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);

    if (connect(s, (struct sockaddr *)&remote, sizeof(remote)) == 0) {
        struct sockaddr_in local;
        socklen_t len = sizeof(local);
        if (getsockname(s, (struct sockaddr *)&local, &len) == 0) {
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)) != NULL) {
                if (strcmp(buf, "0.0.0.0") != 0) {
                    strncpy(out_ip, buf, max_len - 1);
                    out_ip[max_len - 1] = '\0';
                }
            }
        }
    }
    close(s);
}

static const struct {
    const char *host;
    int port;
} g_stun_servers[] = {
    {"stun.l.google.com", 19302},
    {"stun1.l.google.com", 19302},
    {"stun2.l.google.com", 19302},
    {"stun.cloudflare.com", 3478},
    {"stun.matrix.org", 3478}
};

int wex_stun_parse_response(const uint8_t *buf, size_t len, struct sockaddr_in *out_reflexive_addr) {
    if (!buf || len < sizeof(wex_stun_hdr_t) || !out_reflexive_addr) return -1;

    const wex_stun_hdr_t *resp = (const wex_stun_hdr_t *)buf;
    if (ntohs(resp->msg_type) != 0x0101) return -1;
    if (ntohl(resp->magic_cookie) != STUN_MAGIC) return -1;

    size_t attr_offset = sizeof(wex_stun_hdr_t);
    size_t total_payload = len;

    while (attr_offset + 4 <= total_payload) {
        uint16_t attr_type = ntohs(*(const uint16_t *)(buf + attr_offset));
        uint16_t attr_len = ntohs(*(const uint16_t *)(buf + attr_offset + 2));
        attr_offset += 4;

        if (attr_offset + attr_len > total_payload) break;

        if (attr_type == STUN_ATTR_XOR_MAPPED_ADDR && attr_len >= 8) {
            uint8_t family = buf[attr_offset + 1];
            if (family == 0x01) { /* IPv4 */
                uint16_t xor_port = ntohs(*(const uint16_t *)(buf + attr_offset + 2));
                uint32_t xor_ip = *(const uint32_t *)(buf + attr_offset + 4);

                uint16_t real_port = xor_port ^ (uint16_t)(STUN_MAGIC >> 16);
                uint32_t real_ip = xor_ip ^ htonl(STUN_MAGIC);

                memset(out_reflexive_addr, 0, sizeof(*out_reflexive_addr));
                out_reflexive_addr->sin_family = AF_INET;
                out_reflexive_addr->sin_port = htons(real_port);
                out_reflexive_addr->sin_addr.s_addr = real_ip;
                return 0;
            }
        } else if (attr_type == STUN_ATTR_MAPPED_ADDR && attr_len >= 8) {
            uint8_t family = buf[attr_offset + 1];
            if (family == 0x01) { /* IPv4 */
                uint16_t mapped_port = ntohs(*(const uint16_t *)(buf + attr_offset + 2));
                uint32_t mapped_ip = *(const uint32_t *)(buf + attr_offset + 4);

                memset(out_reflexive_addr, 0, sizeof(*out_reflexive_addr));
                out_reflexive_addr->sin_family = AF_INET;
                out_reflexive_addr->sin_port = htons(mapped_port);
                out_reflexive_addr->sin_addr.s_addr = mapped_ip;
                return 0;
            }
        }

        /* Attributes are padded to 4-byte boundaries */
        attr_offset += (attr_len + 3) & ~3;
    }

    return -1;
}

int wex_stun_query(int udp_fd, const char *stun_host, int stun_port,
                   struct sockaddr_in *out_reflexive_addr, int timeout_ms) {
    if (udp_fd < 0 || !stun_host || !out_reflexive_addr) return -1;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", stun_port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(stun_host, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }

    struct sockaddr_in stun_addr;
    memcpy(&stun_addr, res->ai_addr, sizeof(struct sockaddr_in));
    freeaddrinfo(res);

    /* Construct STUN Binding Request */
    wex_stun_hdr_t req;
    memset(&req, 0, sizeof(req));
    req.msg_type = htons(0x0001); /* Binding Request */
    req.msg_len = 0;
    req.magic_cookie = htonl(STUN_MAGIC);
    get_random_bytes(req.transaction_id, sizeof(req.transaction_id));

    ssize_t sent = sendto(udp_fd, &req, sizeof(req), 0,
                          (struct sockaddr *)&stun_addr, sizeof(stun_addr));
    if (sent != (ssize_t)sizeof(req)) {
        return -1;
    }

    /* Wait for response */
    uint64_t start_time = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    start_time = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;

    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t elapsed = ((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL) - start_time;
        if (elapsed >= (uint64_t)timeout_ms) {
            break;
        }

        int remaining = timeout_ms - (int)elapsed;
        struct pollfd pfd;
        pfd.fd = udp_fd;
        pfd.events = POLLIN;

        int pr = poll(&pfd, 1, remaining);
        if (pr <= 0) break;

        uint8_t buf[1024];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(udp_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from_addr, &from_len);
        if (n < (ssize_t)sizeof(wex_stun_hdr_t)) continue;

        wex_stun_hdr_t *resp = (wex_stun_hdr_t *)buf;
        if (ntohs(resp->msg_type) != 0x0101) continue; /* Must be Success Response */
        if (ntohl(resp->magic_cookie) != STUN_MAGIC) continue;
        if (memcmp(resp->transaction_id, req.transaction_id, 12) != 0) continue;

        if (wex_stun_parse_response(buf, (size_t)n, out_reflexive_addr) == 0) {
            return 0;
        }
    }

    return -1;
}

int wex_nat_send_probes(int udp_fd) {
    if (udp_fd < 0) return -1;

    wex_stun_hdr_t req;
    memset(&req, 0, sizeof(req));
    req.msg_type = htons(0x0001);
    req.msg_len = 0;
    req.magic_cookie = htonl(STUN_MAGIC);
    get_random_bytes(req.transaction_id, sizeof(req.transaction_id));

    size_t count = sizeof(g_stun_servers) / sizeof(g_stun_servers[0]);
    int sent = 0;
    for (size_t i = 0; i < count; i++) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", g_stun_servers[i].port);

        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        if (getaddrinfo(g_stun_servers[i].host, port_str, &hints, &res) == 0 && res) {
            ssize_t s = sendto(udp_fd, &req, sizeof(req), 0,
                               res->ai_addr, res->ai_addrlen);
            if (s == (ssize_t)sizeof(req)) {
                sent++;
            }
            freeaddrinfo(res);
        }
    }

    return (sent > 0) ? 0 : -1;
}

int wex_nat_auto_discover(int udp_fd, struct sockaddr_in *out_addr) {
    if (udp_fd < 0 || !out_addr) return -1;

    size_t count = sizeof(g_stun_servers) / sizeof(g_stun_servers[0]);
    for (size_t i = 0; i < count; i++) {
        if (wex_stun_query(udp_fd, g_stun_servers[i].host, g_stun_servers[i].port, out_addr, 500) == 0) {
            return 0;
        }
    }

    return -1;
}
