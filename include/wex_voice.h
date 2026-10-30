#ifndef WEX_VOICE_H
#define WEX_VOICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <netinet/in.h>

#define WEX_VOICE_MAX_FRAME_SIZE 512
#define WEX_VOICE_JITTER_DEPTH 16
#define WEX_VOICE_MAX_PARTICIPANTS 32

#pragma pack(push, 1)
typedef struct {
    uint32_t ssrc;                  /* Speaker identifier */
    uint32_t sequence;              /* Monotonic sequence number */
    uint32_t timestamp;             /* Audio sample timestamp (e.g. 48kHz) */
    char     channel[32];           /* Voice channel identifier */
    uint16_t payload_len;           /* Audio payload length */
} wex_voice_hdr_t;
#pragma pack(pop)

typedef struct {
    wex_voice_hdr_t hdr;
    uint8_t payload[WEX_VOICE_MAX_FRAME_SIZE];
    bool valid;
} wex_voice_frame_t;

/* Circular jitter buffer for network jitter compensation and reordering */
typedef struct {
    wex_voice_frame_t slots[WEX_VOICE_JITTER_DEPTH];
    uint32_t next_play_seq;
    bool initialized;
} wex_jitter_buffer_t;

/* Initialize a jitter buffer */
void wex_jitter_buffer_init(wex_jitter_buffer_t *jb);

/* Push an incoming packet into the jitter buffer; returns 0 on success */
int wex_jitter_buffer_push(wex_jitter_buffer_t *jb, const wex_voice_hdr_t *hdr, const uint8_t *payload, size_t len);

/* Pop the next in-order frame for playback. Returns true if frame found, false if buffer empty or frame dropped */
bool wex_jitter_buffer_pop(wex_jitter_buffer_t *jb, wex_voice_frame_t *out_frame);

/* Voice room session tracking */
typedef struct {
    char channel[32];
    uint32_t my_ssrc;
    uint32_t next_seq;
    uint32_t timestamp_clock;
    struct sockaddr_in participants[WEX_VOICE_MAX_PARTICIPANTS];
    size_t participant_count;
    bool is_in_call;
    wex_jitter_buffer_t jitter;
} wex_voice_room_t;

/* Initialize voice room */
void wex_voice_room_init(wex_voice_room_t *room, const char *channel, uint32_t ssrc);

/* Add participant address */
bool wex_voice_room_add_peer(wex_voice_room_t *room, const struct sockaddr_in *addr);

/* Remove participant */
void wex_voice_room_remove_peer(wex_voice_room_t *room, const struct sockaddr_in *addr);

/* Format an outgoing voice packet header and advance sequence/clock */
void wex_voice_create_header(wex_voice_room_t *room, wex_voice_hdr_t *out_hdr, size_t payload_len);

#endif /* WEX_VOICE_H */
