#ifndef WEX_NAT_H
#define WEX_NAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <netinet/in.h>

#define WEX_STUN_DEFAULT_HOST "stun.l.google.com"
#define WEX_STUN_DEFAULT_PORT 19302

/* STUN RFC 5389 message header */
#pragma pack(push, 1)
typedef struct {
    uint16_t msg_type;       /* 0x0001: Binding Request, 0x0101: Success Response */
    uint16_t msg_len;        /* Length of attributes (excludes 20-byte header) */
    uint32_t magic_cookie;   /* 0x2112A442 */
    uint8_t  transaction_id[12];
} wex_stun_hdr_t;
#pragma pack(pop)

/* Query STUN server over an existing UDP socket to discover external reflexive address */
int wex_stun_query(int udp_fd, const char *stun_host, int stun_port,
                   struct sockaddr_in *out_reflexive_addr, int timeout_ms);

/* Try a list of well-known STUN servers to resolve external IP and port */
int wex_nat_auto_discover(int udp_fd, struct sockaddr_in *out_addr);

/* Send non-blocking STUN binding requests to well-known STUN servers */
int wex_nat_send_probes(int udp_fd);

/* Parse a received STUN response buffer to extract reflexive sockaddr_in */
int wex_stun_parse_response(const uint8_t *buf, size_t len, struct sockaddr_in *out_reflexive_addr);

/* Detect primary local network IPv4 address (e.g. "192.168.1.100") */
void wex_detect_local_lan_ip(char *out_ip, size_t max_len);

/* Format IP and port to string buffer (e.g. "178.54.183.43:44547") */
void wex_addr_to_string(const struct sockaddr_in *addr, char *out_str, size_t max_len);

#endif /* WEX_NAT_H */
