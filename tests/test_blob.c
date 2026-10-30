#define _GNU_SOURCE
#include "wex_blob.h"
#include "wex_p2p.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

typedef struct {
    int completed_count;
    uint8_t last_hash[32];
    size_t last_size;
} blob_sink_t;

static void on_test_blob_completed(const wex_blob_info_t *info, const uint8_t *data, void *user_data) {
    (void)data;
    blob_sink_t *s = (blob_sink_t *)user_data;
    s->completed_count++;
    memcpy(s->last_hash, info->root_hash, 32);
    s->last_size = (size_t)info->total_size;
}

static void test_blob_disk_storage(void) {
    const char *test_dir = "/tmp/wex_test_blob_dir";
    system("rm -rf /tmp/wex_test_blob_dir");

    /* Create sample payload of 3500 bytes (multiple chunks) */
    size_t payload_sz = 3500;
    uint8_t *sample = malloc(payload_sz);
    for (size_t i = 0; i < payload_sz; i++) {
        sample[i] = (uint8_t)((i * 37 + 13) & 0xFF);
    }

    wex_blob_info_t info;
    assert(wex_blob_info_create(&info, "avatar.png", "image/png", sample, payload_sz) == 0);
    assert(info.total_size == payload_sz);
    assert(info.total_chunks == 4); /* 3500 / 1024 = 3 full + 1 partial */

    /* Save to disk */
    assert(wex_blob_save(test_dir, &info, sample) == 0);

    /* Load from disk and verify */
    uint8_t *loaded = NULL;
    size_t loaded_sz = 0;
    assert(wex_blob_load(test_dir, info.root_hash, &loaded, &loaded_sz) == 0);
    assert(loaded_sz == payload_sz);
    assert(memcmp(sample, loaded, payload_sz) == 0);

    free(sample);
    free(loaded);
    system("rm -rf /tmp/wex_test_blob_dir");
    TEST_PASS("Blob Hash, Chunk Calculation, and Disk Persistence");
}

static void test_p2p_blob_transfer(void) {
    const char *dir_a = "/tmp/wex_blob_alice";
    const char *dir_b = "/tmp/wex_blob_bob";
    system("rm -rf /tmp/wex_blob_alice /tmp/wex_blob_bob");

    wex_identity_t id_a, id_b;
    assert(wex_identity_init(&id_a, NULL, "Alice_Sender") == 0);
    assert(wex_identity_init(&id_b, NULL, "Bob_Receiver") == 0);

    wex_p2p_node_t node_a, node_b;
    assert(wex_p2p_init(&node_a, &id_a, 34001) == 0);
    assert(wex_p2p_init(&node_b, &id_b, 34002) == 0);
    wex_p2p_set_data_dir(&node_a, dir_a);
    wex_p2p_set_data_dir(&node_b, dir_b);

    blob_sink_t sink_b = {0};
    wex_p2p_set_blob_callback(&node_b, on_test_blob_completed);
    node_b.user_data = &sink_b;

    /* Connect A and B */
    assert(wex_p2p_connect(&node_a, "127.0.0.1", 34002) == 0);
    for (int t = 0; t < 15; t++) {
        wex_p2p_poll(&node_a, 5);
        wex_p2p_poll(&node_b, 5);
        usleep(5000);
    }
    assert(node_a.peer_count == 1);
    assert(node_b.peer_count == 1);

    /* Alice creates and saves a file of 4200 bytes */
    size_t file_sz = 4200;
    uint8_t *file_data = malloc(file_sz);
    for (size_t i = 0; i < file_sz; i++) {
        file_data[i] = (uint8_t)((i ^ 0xAA) & 0xFF);
    }

    wex_blob_info_t file_info;
    assert(wex_blob_info_create(&file_info, "screenshot.jpg", "image/jpeg", file_data, file_sz) == 0);
    assert(wex_blob_save(dir_a, &file_info, file_data) == 0);

    /* Bob requests download from Alice */
    assert(wex_p2p_download_blob(&node_b, &node_b.peers[0].addr, &file_info) == 0);

    /* Poll network until all chunks transfer and assemble */
    for (int t = 0; t < 50; t++) {
        wex_p2p_poll(&node_a, 5);
        wex_p2p_poll(&node_b, 5);
        usleep(5000);
        if (sink_b.completed_count > 0) break;
    }

    assert(sink_b.completed_count == 1);
    assert(sink_b.last_size == file_sz);
    assert(memcmp(sink_b.last_hash, file_info.root_hash, 32) == 0);

    /* Verify Bob can load the saved blob from his own local disk */
    uint8_t *bob_loaded = NULL;
    size_t bob_sz = 0;
    assert(wex_blob_load(dir_b, file_info.root_hash, &bob_loaded, &bob_sz) == 0);
    assert(bob_sz == file_sz);
    assert(memcmp(file_data, bob_loaded, file_sz) == 0);

    free(file_data);
    free(bob_loaded);
    wex_p2p_close(&node_a);
    wex_p2p_close(&node_b);
    system("rm -rf /tmp/wex_blob_alice /tmp/wex_blob_bob");

    TEST_PASS("Multi-chunk P2P Blob Transfer over UDP with Root Hash Assembly");
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex Blob & File Transfer Tests            \n");
    printf("====================================================\n");

    test_blob_disk_storage();
    test_p2p_blob_transfer();

    printf("====================================================\n");
    printf("  ALL BLOB TRANSFER TESTS PASSED SUCCESSFULLY!       \n");
    printf("====================================================\n");
    return 0;
}
