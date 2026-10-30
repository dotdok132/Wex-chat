#define _GNU_SOURCE
#include "wex_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void wex_config_set_defaults(wex_config_t *conf) {
    if (!conf) return;
    memset(conf, 0, sizeof(*conf));
    strncpy(conf->nickname, "dotdok", sizeof(conf->nickname) - 1);
    conf->udp_port = 9001;
    conf->ws_port = 9000;
    strncpy(conf->data_dir, "./.wex_data", sizeof(conf->data_dir) - 1);
    conf->is_seed = false;
    conf->bootstrap_count = 0;
}

static char *trim_whitespace(char *str) {
    if (!str) return NULL;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;

    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = 0;
        end--;
    }
    return str;
}

static void parse_bootstrap_list(wex_config_t *conf, const char *val) {
    if (!conf || !val) return;
    char buffer[512];
    strncpy(buffer, val, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buffer, ", \t", &saveptr);
    while (token && conf->bootstrap_count < WEX_CONF_MAX_BOOTSTRAP) {
        char *entry = trim_whitespace(token);
        if (*entry) {
            char *colon = strchr(entry, ':');
            if (colon) {
                *colon = '\0';
                int port = atoi(colon + 1);
                if (port > 0 && port <= 65535) {
                    wex_bootstrap_entry_t *b = &conf->bootstrap_peers[conf->bootstrap_count++];
                    strncpy(b->host, entry, sizeof(b->host) - 1);
                    b->port = port;
                }
            }
        }
        token = strtok_r(NULL, ", \t", &saveptr);
    }
}

int wex_config_load_file(wex_config_t *conf, const char *path) {
    if (!conf || !path) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim_whitespace(line);
        if (*trimmed == '\0' || *trimmed == '#' || (trimmed[0] == '/' && trimmed[1] == '/')) {
            continue;
        }

        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim_whitespace(trimmed);
        char *val = trim_whitespace(eq + 1);

        if (strcasecmp(key, "nickname") == 0 || strcasecmp(key, "nick") == 0) {
            strncpy(conf->nickname, val, sizeof(conf->nickname) - 1);
            conf->nickname[sizeof(conf->nickname) - 1] = '\0';
        } else if (strcasecmp(key, "udp_port") == 0 || strcasecmp(key, "port") == 0) {
            conf->udp_port = atoi(val);
        } else if (strcasecmp(key, "ws_port") == 0 || strcasecmp(key, "ws") == 0) {
            conf->ws_port = atoi(val);
        } else if (strcasecmp(key, "data_dir") == 0) {
            strncpy(conf->data_dir, val, sizeof(conf->data_dir) - 1);
            conf->data_dir[sizeof(conf->data_dir) - 1] = '\0';
        } else if (strcasecmp(key, "seed_mode") == 0 || strcasecmp(key, "seed") == 0 || strcasecmp(key, "is_seed") == 0) {
            conf->is_seed = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0 || strcasecmp(val, "yes") == 0);
        } else if (strcasecmp(key, "bootstrap") == 0 || strcasecmp(key, "bootstrap_nodes") == 0 || strcasecmp(key, "peers") == 0) {
            parse_bootstrap_list(conf, val);
        }
    }

    fclose(f);
    return 0;
}

int wex_config_save_file(const wex_config_t *conf, const char *path) {
    if (!conf || !path) return -1;

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "# Wex-Chat Node Configuration\n");
    fprintf(f, "# Sovereign Privacy-First P2P Messenger\n\n");

    fprintf(f, "nickname = %s\n", conf->nickname);
    fprintf(f, "udp_port = %d\n", conf->udp_port);
    fprintf(f, "ws_port = %d\n", conf->ws_port);
    fprintf(f, "data_dir = %s\n", conf->data_dir);
    fprintf(f, "seed_mode = %s\n\n", conf->is_seed ? "true" : "false");

    fprintf(f, "# Comma-separated list of bootstrap peers (host:port)\n");
    fprintf(f, "bootstrap_nodes = ");
    for (size_t i = 0; i < conf->bootstrap_count; i++) {
        fprintf(f, "%s:%d%s", conf->bootstrap_peers[i].host, conf->bootstrap_peers[i].port,
                (i + 1 < conf->bootstrap_count) ? ", " : "");
    }
    fprintf(f, "\n");

    fclose(f);
    return 0;
}
