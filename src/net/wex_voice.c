#define _GNU_SOURCE
#include "wex_voice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wex_jitter_buffer_init(wex_jitter_buffer_t *jb) {
    if (!jb) return;
    memset(jb, 0, sizeof(*jb));
    jb->next_play_seq = 0;
    jb->initialized = false;
}

int wex_jitter_buffer_push(wex_jitter_buffer_t *jb, const wex_voice_hdr_t *hdr, const uint8_t *payload, size_t len) {
    if (!jb || !hdr || (len > 0 && !payload) || len > WEX_VOICE_MAX_FRAME_SIZE) {
        return -1;
    }

    if (!jb->initialized) {
        jb->next_play_seq = hdr->sequence;
        jb->initialized = true;
    }

    /* Check if packet is too old to be played */
    if (hdr->sequence < jb->next_play_seq) {
        return 0; /* Stale packet dropped */
    }

    /* Check if packet is too far in future beyond buffer depth */
    if (hdr->sequence >= jb->next_play_seq + WEX_VOICE_JITTER_DEPTH) {
        /* Buffer underrun / desync: resynchronize to current burst */
        jb->next_play_seq = hdr->sequence;
        for (int i = 0; i < WEX_VOICE_JITTER_DEPTH; i++) {
            jb->slots[i].valid = false;
        }
    }

    size_t slot_idx = hdr->sequence % WEX_VOICE_JITTER_DEPTH;
    jb->slots[slot_idx].hdr = *hdr;
    if (len > 0) {
        memcpy(jb->slots[slot_idx].payload, payload, len);
    }
    jb->slots[slot_idx].valid = true;

    return 0;
}

bool wex_jitter_buffer_pop(wex_jitter_buffer_t *jb, wex_voice_frame_t *out_frame) {
    if (!jb || !out_frame || !jb->initialized) return false;

    size_t slot_idx = jb->next_play_seq % WEX_VOICE_JITTER_DEPTH;
    if (jb->slots[slot_idx].valid && jb->slots[slot_idx].hdr.sequence == jb->next_play_seq) {
        *out_frame = jb->slots[slot_idx];
        jb->slots[slot_idx].valid = false;
        jb->next_play_seq++;
        return true;
    }

    /* Check if newer packets exist ahead */
    bool has_future = false;
    for (size_t i = 1; i < WEX_VOICE_JITTER_DEPTH; i++) {
        size_t next_idx = (jb->next_play_seq + i) % WEX_VOICE_JITTER_DEPTH;
        if (jb->slots[next_idx].valid && jb->slots[next_idx].hdr.sequence == jb->next_play_seq + i) {
            has_future = true;
            break;
        }
    }

    if (has_future) {
        /* Packet was lost in transit: skip missing slot to avoid stutter */
        jb->slots[slot_idx].valid = false;
        jb->next_play_seq++;
    }

    return false;
}

void wex_voice_room_init(wex_voice_room_t *room, const char *channel, uint32_t ssrc) {
    if (!room) return;
    memset(room, 0, sizeof(*room));
    if (channel) {
        strncpy(room->channel, channel, sizeof(room->channel) - 1);
    } else {
        strncpy(room->channel, "voice-general", sizeof(room->channel) - 1);
    }
    room->my_ssrc = ssrc;
    room->next_seq = 1;
    room->timestamp_clock = 0;
    room->participant_count = 0;
    room->is_in_call = true;
    wex_jitter_buffer_init(&room->jitter);
}

bool wex_voice_room_add_peer(wex_voice_room_t *room, const struct sockaddr_in *addr) {
    if (!room || !addr) return false;

    for (size_t i = 0; i < room->participant_count; i++) {
        if (room->participants[i].sin_addr.s_addr == addr->sin_addr.s_addr &&
            room->participants[i].sin_port == addr->sin_port) {
            return true; /* Already added */
        }
    }

    if (room->participant_count < WEX_VOICE_MAX_PARTICIPANTS) {
        room->participants[room->participant_count++] = *addr;
        return true;
    }

    return false;
}

void wex_voice_room_remove_peer(wex_voice_room_t *room, const struct sockaddr_in *addr) {
    if (!room || !addr) return;

    for (size_t i = 0; i < room->participant_count; i++) {
        if (room->participants[i].sin_addr.s_addr == addr->sin_addr.s_addr &&
            room->participants[i].sin_port == addr->sin_port) {
            room->participants[i] = room->participants[room->participant_count - 1];
            room->participant_count--;
            break;
        }
    }
}

void wex_voice_create_header(wex_voice_room_t *room, wex_voice_hdr_t *out_hdr, size_t payload_len) {
    if (!room || !out_hdr) return;

    memset(out_hdr, 0, sizeof(*out_hdr));
    out_hdr->ssrc = room->my_ssrc;
    out_hdr->sequence = room->next_seq++;
    out_hdr->timestamp = room->timestamp_clock;
    strncpy(out_hdr->channel, room->channel, sizeof(out_hdr->channel) - 1);
    out_hdr->payload_len = (uint16_t)payload_len;

    /* Advance clock by 960 audio samples (20 ms frame @ 48 kHz standard) */
    room->timestamp_clock += 960;
}
