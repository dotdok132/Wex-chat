#define _GNU_SOURCE
#include "wex_voice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

static void test_voice_room_and_framing(void) {
    wex_voice_room_t room;
    wex_voice_room_init(&room, "lounge", 0xA1B2C3D4);

    assert(strcmp(room.channel, "lounge") == 0);
    assert(room.my_ssrc == 0xA1B2C3D4);
    assert(room.is_in_call == false);
    assert(room.participant_count == 0);

    /* Joining voice call activates in-call status */
    room.is_in_call = true;
    assert(room.is_in_call == true);

    /* Add participants */
    struct sockaddr_in p1, p2;
    memset(&p1, 0, sizeof(p1));
    p1.sin_family = AF_INET;
    p1.sin_port = htons(9001);
    inet_pton(AF_INET, "127.0.0.1", &p1.sin_addr);

    memset(&p2, 0, sizeof(p2));
    p2.sin_family = AF_INET;
    p2.sin_port = htons(9002);
    inet_pton(AF_INET, "127.0.0.1", &p2.sin_addr);

    assert(wex_voice_room_add_peer(&room, &p1) == true);
    assert(wex_voice_room_add_peer(&room, &p2) == true);
    assert(wex_voice_room_add_peer(&room, &p1) == true); /* duplicate should not increase */
    assert(room.participant_count == 2);

    /* Generate voice frame headers */
    wex_voice_hdr_t h1, h2;
    wex_voice_create_header(&room, &h1, 80);
    wex_voice_create_header(&room, &h2, 80);

    assert(h1.ssrc == 0xA1B2C3D4);
    assert(h1.sequence == 1);
    assert(h1.timestamp == 0);
    assert(h1.payload_len == 80);

    assert(h2.ssrc == 0xA1B2C3D4);
    assert(h2.sequence == 2);
    assert(h2.timestamp == 960); /* 48 kHz standard 20ms audio frame */
    assert(h2.payload_len == 80);

    /* Remove peer and leave call */
    wex_voice_room_remove_peer(&room, &p1);
    assert(room.participant_count == 1);
    room.is_in_call = false;
    assert(room.is_in_call == false);

    TEST_PASS("Voice Room Session, Framing, and Timestamp Clock");
}

static void test_jitter_buffer_in_order(void) {
    wex_jitter_buffer_t jb;
    wex_jitter_buffer_init(&jb);

    uint8_t opus_data[3][4] = {
        {0xAA, 0x11, 0x22, 0x33},
        {0xBB, 0x44, 0x55, 0x66},
        {0xCC, 0x77, 0x88, 0x99}
    };

    for (uint32_t i = 1; i <= 3; i++) {
        wex_voice_hdr_t hdr = {
            .ssrc = 1234,
            .sequence = i,
            .timestamp = (i - 1) * 960,
            .payload_len = 4
        };
        assert(wex_jitter_buffer_push(&jb, &hdr, opus_data[i - 1], 4) == 0);
    }

    /* Pop sequentially */
    for (uint32_t i = 1; i <= 3; i++) {
        wex_voice_frame_t frame;
        assert(wex_jitter_buffer_pop(&jb, &frame) == true);
        assert(frame.hdr.sequence == i);
        assert(memcmp(frame.payload, opus_data[i - 1], 4) == 0);
    }

    wex_voice_frame_t empty_frame;
    assert(wex_jitter_buffer_pop(&jb, &empty_frame) == false);

    TEST_PASS("Jitter Buffer Sequential In-Order Playback");
}

static void test_jitter_buffer_reordering(void) {
    wex_jitter_buffer_t jb;
    wex_jitter_buffer_init(&jb);

    /* Arriving out-of-order over UDP: sequence 3, then 1, then 2 */
    wex_voice_hdr_t h3 = { .ssrc = 1, .sequence = 3, .timestamp = 1920, .payload_len = 2 };
    uint8_t d3[2] = {0x03, 0x03};

    wex_voice_hdr_t h1 = { .ssrc = 1, .sequence = 1, .timestamp = 0, .payload_len = 2 };
    uint8_t d1[2] = {0x01, 0x01};

    wex_voice_hdr_t h2 = { .ssrc = 1, .sequence = 2, .timestamp = 960, .payload_len = 2 };
    uint8_t d2[2] = {0x02, 0x02};

    /* In order of UDP arrival */
    assert(wex_jitter_buffer_push(&jb, &h1, d1, 2) == 0);
    assert(wex_jitter_buffer_push(&jb, &h3, d3, 2) == 0);
    assert(wex_jitter_buffer_push(&jb, &h2, d2, 2) == 0);

    /* Pop must come out strictly sorted: 1, 2, 3 */
    wex_voice_frame_t f1, f2, f3;
    assert(wex_jitter_buffer_pop(&jb, &f1) == true);
    assert(f1.hdr.sequence == 1);
    assert(f1.payload[0] == 0x01);

    assert(wex_jitter_buffer_pop(&jb, &f2) == true);
    assert(f2.hdr.sequence == 2);
    assert(f2.payload[0] == 0x02);

    assert(wex_jitter_buffer_pop(&jb, &f3) == true);
    assert(f3.hdr.sequence == 3);
    assert(f3.payload[0] == 0x03);

    TEST_PASS("Jitter Buffer Automatic Packet Reordering");
}

static void test_jitter_buffer_lost_packet(void) {
    wex_jitter_buffer_t jb;
    wex_jitter_buffer_init(&jb);

    /* Packet 1 arrives */
    wex_voice_hdr_t h1 = { .ssrc = 1, .sequence = 1, .timestamp = 0, .payload_len = 1 };
    uint8_t d1 = 0xAA;
    assert(wex_jitter_buffer_push(&jb, &h1, &d1, 1) == 0);

    /* Packet 2 is LOST on the wire! */

    /* Packet 3 arrives */
    wex_voice_hdr_t h3 = { .ssrc = 1, .sequence = 3, .timestamp = 1920, .payload_len = 1 };
    uint8_t d3 = 0xCC;
    assert(wex_jitter_buffer_push(&jb, &h3, &d3, 1) == 0);

    /* Pop frame 1 */
    wex_voice_frame_t frame;
    assert(wex_jitter_buffer_pop(&jb, &frame) == true);
    assert(frame.hdr.sequence == 1);

    /* Next pop: frame 2 is missing, jitter buffer skips gap */
    assert(wex_jitter_buffer_pop(&jb, &frame) == false);

    /* Following pop: frame 3 is successfully retrieved without stall */
    assert(wex_jitter_buffer_pop(&jb, &frame) == true);
    assert(frame.hdr.sequence == 3);
    assert(frame.payload[0] == 0xCC);

    TEST_PASS("Jitter Buffer Resilient Packet Loss Handling without Stalling");
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex Voice Engine & Jitter Buffer Tests    \n");
    printf("====================================================\n");

    test_voice_room_and_framing();
    test_jitter_buffer_in_order();
    test_jitter_buffer_reordering();
    test_jitter_buffer_lost_packet();

    printf("====================================================\n");
    printf("  ALL VOICE TESTS PASSED SUCCESSFULLY!              \n");
    printf("====================================================\n");
    return 0;
}
