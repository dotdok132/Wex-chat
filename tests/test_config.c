#define _GNU_SOURCE
#include "wex_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

static void test_config_defaults(void) {
    wex_config_t conf;
    wex_config_set_defaults(&conf);

    assert(strcmp(conf.nickname, "dotdok") == 0);
    assert(conf.udp_port == 9001);
    assert(conf.ws_port == 9000);
    assert(strcmp(conf.data_dir, "./.wex_data") == 0);
    assert(conf.is_seed == false);
    assert(conf.bootstrap_count == 0);

    TEST_PASS("Config Default Initialization");
}

static void test_config_save_and_load(void) {
    const char *test_path = "/tmp/test_wex.conf";
    unlink(test_path);

    wex_config_t conf_orig;
    wex_config_set_defaults(&conf_orig);
    strncpy(conf_orig.nickname, "SovereignNode_01", sizeof(conf_orig.nickname) - 1);
    conf_orig.udp_port = 45001;
    conf_orig.ws_port = 45000;
    strncpy(conf_orig.data_dir, "/tmp/custom_data", sizeof(conf_orig.data_dir) - 1);
    conf_orig.is_seed = true;

    /* Add bootstrap peers */
    strncpy(conf_orig.bootstrap_peers[0].host, "127.0.0.1", 63);
    conf_orig.bootstrap_peers[0].port = 9001;
    strncpy(conf_orig.bootstrap_peers[1].host, "node.wex.network", 63);
    conf_orig.bootstrap_peers[1].port = 9002;
    conf_orig.bootstrap_count = 2;

    assert(wex_config_save_file(&conf_orig, test_path) == 0);

    /* Load and verify */
    wex_config_t conf_loaded;
    assert(wex_config_load_file(&conf_loaded, test_path) == 0);

    assert(strcmp(conf_loaded.nickname, "SovereignNode_01") == 0);
    assert(conf_loaded.udp_port == 45001);
    assert(conf_loaded.ws_port == 45000);
    assert(strcmp(conf_loaded.data_dir, "/tmp/custom_data") == 0);
    assert(conf_loaded.is_seed == true);
    assert(conf_loaded.bootstrap_count == 2);
    assert(strcmp(conf_loaded.bootstrap_peers[0].host, "127.0.0.1") == 0);
    assert(conf_loaded.bootstrap_peers[0].port == 9001);
    assert(strcmp(conf_loaded.bootstrap_peers[1].host, "node.wex.network") == 0);
    assert(conf_loaded.bootstrap_peers[1].port == 9002);

    unlink(test_path);
    TEST_PASS("Config File Save, Comments Parsing, and Load Verification");
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex Configuration Tests                   \n");
    printf("====================================================\n");

    test_config_defaults();
    test_config_save_and_load();

    printf("====================================================\n");
    printf("  ALL CONFIG TESTS PASSED SUCCESSFULLY!             \n");
    printf("====================================================\n");
    return 0;
}
