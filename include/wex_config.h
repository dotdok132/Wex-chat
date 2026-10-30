#ifndef WEX_CONFIG_H
#define WEX_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define WEX_CONF_MAX_BOOTSTRAP 16
#define WEX_CONF_STR_LEN 256

typedef struct {
    char host[128];
    int port;
} wex_bootstrap_entry_t;

typedef struct {
    char nickname[64];
    int udp_port;
    int ws_port;
    char data_dir[WEX_CONF_STR_LEN];
    bool is_seed;

    /* Bootstrap peers list */
    wex_bootstrap_entry_t bootstrap_peers[WEX_CONF_MAX_BOOTSTRAP];
    size_t bootstrap_count;
} wex_config_t;

/* Initialize config struct with sensible defaults */
void wex_config_set_defaults(wex_config_t *conf);

/* Load configuration from file (supports key = value format, # and // comments) */
int wex_config_load_file(wex_config_t *conf, const char *path);

/* Save current configuration to file */
int wex_config_save_file(const wex_config_t *conf, const char *path);

#endif /* WEX_CONFIG_H */
