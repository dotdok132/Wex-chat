#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "../../vendor/cjson/cJSON.h"
#include "../../vendor/monocypher/monocypher.h"

#define CLI_BUF_SIZE 8192

static int connect_ws(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Perform simple client handshake */
    const char *req = 
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";

    if (send(fd, req, strlen(req), 0) < 0) {
        close(fd);
        return -1;
    }

    char resp[1024];
    ssize_t r = recv(fd, resp, sizeof(resp) - 1, 0);
    if (r <= 0 || !strstr(resp, "101 Switching Protocols")) {
        close(fd);
        return -1;
    }

    return fd;
}

static void send_ws_text(int fd, const char *text) {
    size_t len = strlen(text);
    uint8_t frame[CLI_BUF_SIZE];
    frame[0] = 0x81; /* FIN + opcode 1 (text) */

    /* Client frames MUST be masked (RFC 6455) */
    uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    size_t offset = 0;

    if (len <= 125) {
        frame[1] = 0x80 | (uint8_t)len;
        memcpy(frame + 2, mask, 4);
        offset = 6;
    } else if (len <= 65535) {
        frame[1] = 0x80 | 126;
        frame[2] = (uint8_t)((len >> 8) & 0xFF);
        frame[3] = (uint8_t)(len & 0xFF);
        memcpy(frame + 4, mask, 4);
        offset = 8;
    } else {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        frame[offset + i] = (uint8_t)(text[i] ^ mask[i % 4]);
    }

    send(fd, frame, offset + len, 0);
}

static void print_banner(void) {
    printf("\033[1;36m");
    printf("=====================================================\n");
    printf("   __      __             _____ _           _        \n");
    printf("   \\ \\    / /            / ____| |         | |       \n");
    printf("    \\ \\  / /____  __ ___| |    | |__   __ _| |_      \n");
    printf("     \\ \\/ / _ \\ \\/ /____| |    | '_ \\ / _` | __|     \n");
    printf("      \\  /  __/>  <     | |____| | | | (_| | |_      \n");
    printf("       \\/ \\___/_/\\_\\     \\_____|_| |_|\\__,_|\\__|     \n");
    printf("    Sovereign P2P Anti-Surveillance Terminal CLI     \n");
    printf("=====================================================\n");
    printf("\033[0m");
    printf("\033[90mCommands: /switch <ch> | /dm <pk> <msg> | /react <sig> <emoji> | /help | /quit\033[0m\n\n");
}

int main(int argc, char **argv) {
    int ws_port = 9000;
    if (argc > 1) {
        ws_port = atoi(argv[1]);
        if (ws_port <= 0) ws_port = 9000;
    }

    print_banner();
    printf("[*] Connecting to Wex-Chat daemon on ws://127.0.0.1:%d ...\n", ws_port);

    int ws_fd = connect_ws(ws_port);
    if (ws_fd < 0) {
        fprintf(stderr, "\033[1;31m[-] Error: Could not connect to daemon on port %d. Is wex_daemon running?\033[0m\n", ws_port);
        return 1;
    }
    printf("\033[1;32m[+] Connected to local P2P daemon!\033[0m\n\n");

    /* Request initial status */
    send_ws_text(ws_fd, "{\"action\":\"get_status\"}");

    char current_channel[64] = "general";
    char current_space_id[65] = "";

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN;
    fds[1].fd = ws_fd;
    fds[1].events = POLLIN;

    char line_buf[1024];

    while (1) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* 1. Terminal user input */
        if (fds[0].revents & POLLIN) {
            if (!fgets(line_buf, sizeof(line_buf), stdin)) {
                break;
            }
            size_t llen = strlen(line_buf);
            while (llen > 0 && (line_buf[llen - 1] == '\n' || line_buf[llen - 1] == '\r')) {
                line_buf[--llen] = '\0';
            }
            if (llen == 0) continue;

            if (strcmp(line_buf, "/quit") == 0 || strcmp(line_buf, "/exit") == 0) {
                printf("[*] Exiting Wex-Chat CLI.\n");
                break;
            } else if (strncmp(line_buf, "/switch ", 8) == 0) {
                const char *new_ch = line_buf + 8;
                while (*new_ch == ' ') new_ch++;
                strncpy(current_channel, new_ch, sizeof(current_channel) - 1);
                cJSON *cmd = cJSON_CreateObject();
                cJSON_AddStringToObject(cmd, "action", "switch_channel");
                cJSON_AddStringToObject(cmd, "name", current_channel);
                char *s = cJSON_PrintUnformatted(cmd);
                if (s) {
                    send_ws_text(ws_fd, s);
                    free(s);
                }
                cJSON_Delete(cmd);
                printf("\033[1;35m[*] Switched to channel #%s\033[0m\n", current_channel);
            } else if (strncmp(line_buf, "/dm ", 4) == 0) {
                char target_pk[128];
                char dm_text[512];
                if (sscanf(line_buf + 4, "%127s %511[^\n]", target_pk, dm_text) == 2) {
                    cJSON *cmd = cJSON_CreateObject();
                    cJSON_AddStringToObject(cmd, "action", "send_dm");
                    cJSON_AddStringToObject(cmd, "target_pk", target_pk);
                    cJSON_AddStringToObject(cmd, "text", dm_text);
                    char *s = cJSON_PrintUnformatted(cmd);
                    if (s) {
                        send_ws_text(ws_fd, s);
                        free(s);
                    }
                    cJSON_Delete(cmd);
                    printf("\033[1;32m[DM -> %.16s...] %s\033[0m\n", target_pk, dm_text);
                } else {
                    printf("\033[1;31mUsage: /dm <public_key_hex> <message>\033[0m\n");
                }
            } else if (strncmp(line_buf, "/react ", 7) == 0) {
                char tgt_sig[130];
                char emoji[32];
                if (sscanf(line_buf + 7, "%129s %31s", tgt_sig, emoji) == 2) {
                    cJSON *cmd = cJSON_CreateObject();
                    cJSON_AddStringToObject(cmd, "action", "send_reaction");
                    cJSON_AddStringToObject(cmd, "target_sig", tgt_sig);
                    cJSON_AddStringToObject(cmd, "emoji", emoji);
                    char *s = cJSON_PrintUnformatted(cmd);
                    if (s) {
                        send_ws_text(ws_fd, s);
                        free(s);
                    }
                    cJSON_Delete(cmd);
                    printf("\033[1;33m[*] Reaction %s sent to message %.12s...\033[0m\n", emoji, tgt_sig);
                } else {
                    printf("\033[1;31mUsage: /react <signature_hex> <emoji>\033[0m\n");
                }
            } else if (strcmp(line_buf, "/help") == 0) {
                printf("\033[1;33m=== Wex-Chat CLI Help ===\033[0m\n");
                printf("  <text>                 Send chat message to active channel\n");
                printf("  /switch <channel>      Switch active channel (e.g. dev, offtopic)\n");
                printf("  /dm <pk_hex> <text>    Send end-to-end encrypted direct message\n");
                printf("  /react <sig> <emoji>   Add/toggle emoji reaction\n");
                printf("  /quit                  Exit terminal client\n");
            } else {
                /* Regular chat message */
                cJSON *cmd = cJSON_CreateObject();
                cJSON_AddStringToObject(cmd, "action", "send_message");
                cJSON_AddStringToObject(cmd, "space_id", current_space_id);
                cJSON_AddStringToObject(cmd, "channel", current_channel);
                cJSON_AddStringToObject(cmd, "text", line_buf);
                char *s = cJSON_PrintUnformatted(cmd);
                if (s) {
                    send_ws_text(ws_fd, s);
                    free(s);
                }
                cJSON_Delete(cmd);
            }
        }

        /* 2. Incoming WebSocket frames from daemon */
        if (fds[1].revents & POLLIN) {
            uint8_t hdr[2];
            ssize_t n = recv(ws_fd, hdr, 2, MSG_PEEK);
            if (n <= 0) {
                printf("\033[1;31m[-] Daemon closed connection.\033[0m\n");
                break;
            }

            uint8_t raw[CLI_BUF_SIZE];
            ssize_t rlen = recv(ws_fd, raw, sizeof(raw) - 1, 0);
            if (rlen <= 2) continue;

            /* Parse WebSocket unmasked server frame */
            size_t payload_len = raw[1] & 0x7F;
            size_t hlen = 2;
            if (payload_len == 126 && rlen >= 4) {
                payload_len = ((size_t)raw[2] << 8) | raw[3];
                hlen = 4;
            }
            if (rlen < (ssize_t)(hlen + payload_len)) payload_len = (size_t)(rlen - hlen);

            char *json_txt = malloc(payload_len + 1);
            if (!json_txt) continue;
            memcpy(json_txt, raw + hlen, payload_len);
            json_txt[payload_len] = '\0';

            cJSON *evt = cJSON_Parse(json_txt);
            if (evt) {
                cJSON *type = cJSON_GetObjectItem(evt, "type");
                if (type && cJSON_IsString(type)) {
                    if (strcmp(type->valuestring, "message") == 0) {
                        cJSON *author = cJSON_GetObjectItem(evt, "author");
                        cJSON *channel = cJSON_GetObjectItem(evt, "channel");
                        cJSON *content = cJSON_GetObjectItem(evt, "content");
                        cJSON *sig = cJSON_GetObjectItem(evt, "signature");

                        const char *auth_s = author ? author->valuestring : "Unknown";
                        const char *chan_s = channel ? channel->valuestring : "general";
                        const char *cont_s = content ? content->valuestring : "";
                        const char *sig_s = sig ? sig->valuestring : "";

                        printf("\033[1;34m[%s]\033[0m \033[1;36m#%s:\033[0m %s  \033[90m(sig: %.8s...)\033[0m\n",
                               auth_s, chan_s, cont_s, sig_s);
                    } else if (strcmp(type->valuestring, "status") == 0) {
                        cJSON *ident = cJSON_GetObjectItem(evt, "identity");
                        cJSON *spaces = cJSON_GetObjectItem(evt, "spaces");
                        if (ident) {
                            cJSON *nick = cJSON_GetObjectItem(ident, "nickname");
                            cJSON *port = cJSON_GetObjectItem(ident, "udp_port");
                            printf("\033[1;32m[i] Identity: %s | UDP Port: %d\033[0m\n",
                                   nick ? nick->valuestring : "?", port ? port->valueint : 0);
                        }
                        if (spaces && cJSON_GetArraySize(spaces) > 0) {
                            cJSON *first_sp = cJSON_GetArrayItem(spaces, 0);
                            cJSON *sp_id = cJSON_GetObjectItem(first_sp, "id");
                            cJSON *sp_name = cJSON_GetObjectItem(first_sp, "name");
                            if (sp_id && cJSON_IsString(sp_id)) {
                                strncpy(current_space_id, sp_id->valuestring, sizeof(current_space_id) - 1);
                            }
                            printf("\033[1;35m[i] Space: %s [%.12s...]\033[0m\n",
                                   sp_name ? sp_name->valuestring : "Realm", current_space_id);
                        }
                    } else if (strcmp(type->valuestring, "peer_joined") == 0) {
                        cJSON *nick = cJSON_GetObjectItem(evt, "nickname");
                        printf("\033[1;32m● Peer joined: %s\033[0m\n", nick ? nick->valuestring : "peer");
                    } else if (strcmp(type->valuestring, "peer_left") == 0) {
                        cJSON *nick = cJSON_GetObjectItem(evt, "nickname");
                        printf("\033[1;31m○ Peer disconnected: %s\033[0m\n", nick ? nick->valuestring : "peer");
                    } else if (strcmp(type->valuestring, "reaction_updated") == 0) {
                        cJSON *summary = cJSON_GetObjectItem(evt, "summary");
                        cJSON *tsig = cJSON_GetObjectItem(evt, "target_sig");
                        printf("\033[1;33m[Reactions on %.8s...]: %s\033[0m\n",
                               tsig ? tsig->valuestring : "", summary ? summary->valuestring : "");
                    } else if (strcmp(type->valuestring, "message_edited") == 0) {
                        cJSON *tsig = cJSON_GetObjectItem(evt, "target_sig");
                        cJSON *cont = cJSON_GetObjectItem(evt, "content");
                        printf("\033[1;33m[Edited %.8s...]: %s\033[0m\n",
                               tsig ? tsig->valuestring : "", cont ? cont->valuestring : "");
                    } else if (strcmp(type->valuestring, "message_deleted") == 0) {
                        cJSON *tsig = cJSON_GetObjectItem(evt, "target_sig");
                        printf("\033[1;31m[Deleted message %.8s...]\033[0m\n",
                               tsig ? tsig->valuestring : "");
                    }
                }
                cJSON_Delete(evt);
            }
            free(json_txt);
        }
    }

    close(ws_fd);
    return 0;
}
