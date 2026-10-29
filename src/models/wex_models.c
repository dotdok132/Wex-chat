#include "wex_models.h"
#include "wex_crypto.h"
#include "../../vendor/monocypher/monocypher.h"
#include "../../vendor/monocypher/monocypher-ed25519.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void wex_id_to_hex(const uint8_t id[32], char hex[65]) {
    for (int i = 0; i < 32; i++) {
        sprintf(hex + (i * 2), "%02x", id[i]);
    }
    hex[64] = '\0';
}

int wex_hex_to_id(const char hex[64], uint8_t id[32]) {
    for (int i = 0; i < 32; i++) {
        unsigned int val;
        if (sscanf(hex + (i * 2), "%02x", &val) != 1) {
            return -1;
        }
        id[i] = (uint8_t)val;
    }
    return 0;
}

int wex_identity_init(wex_identity_t *ident, const uint8_t *seed, const char *nickname) {
    if (!ident) {
        return -1;
    }

    if (seed) {
        memcpy(ident->seed, seed, 32);
    } else {
        if (wex_randombytes(ident->seed, 32) != 0) {
            return -1;
        }
    }

    /* Derive Ed25519 keypair from seed */
    crypto_ed25519_key_pair(ident->sign_sk, ident->sign_pk, ident->seed);

    if (nickname && strlen(nickname) > 0) {
        strncpy(ident->nickname, nickname, sizeof(ident->nickname) - 1);
        ident->nickname[sizeof(ident->nickname) - 1] = '\0';
    } else {
        strncpy(ident->nickname, "SovereignUser", sizeof(ident->nickname) - 1);
    }

    strncpy(ident->status, "online", sizeof(ident->status) - 1);
    return 0;
}

int wex_space_create(wex_space_t *space, const char *name) {
    if (!space) {
        return -1;
    }

    if (wex_randombytes(space->space_id, WEX_ID_LEN) != 0) {
        return -1;
    }

    if (name && strlen(name) > 0) {
        strncpy(space->name, name, sizeof(space->name) - 1);
        space->name[sizeof(space->name) - 1] = '\0';
    } else {
        strncpy(space->name, "Wex Community", sizeof(space->name) - 1);
    }

    space->channel_count = 0;
    wex_space_add_channel(space, "general");
    wex_space_add_channel(space, "dev");
    return 0;
}

int wex_space_add_channel(wex_space_t *space, const char *channel_name) {
    if (!space || !channel_name || space->channel_count >= WEX_MAX_CHANNELS) {
        return -1;
    }

    strncpy(space->channels[space->channel_count].name, channel_name, WEX_NAME_LEN - 1);
    space->channels[space->channel_count].name[WEX_NAME_LEN - 1] = '\0';
    space->channel_count++;
    return 0;
}

int wex_space_get_invite(const wex_space_t *space, const char *peer_addr, char *out_buf, size_t out_size) {
    if (!space || !out_buf || out_size < 128) {
        return -1;
    }

    char hex[65];
    wex_id_to_hex(space->space_id, hex);

    if (peer_addr && strlen(peer_addr) > 0) {
        snprintf(out_buf, out_size, "wex://join/%s?peer=%s&name=%s", hex, peer_addr, space->name);
    } else {
        snprintf(out_buf, out_size, "wex://join/%s?name=%s", hex, space->name);
    }

    return 0;
}

static size_t format_msg_sign_payload(const wex_chat_msg_t *msg, uint8_t *buf, size_t max_buf) {
    /* Payload = space_id (32B) + channel + timestamp_ms (8B) + content */
    size_t offset = 0;

    memcpy(buf + offset, msg->space_id, WEX_ID_LEN);
    offset += WEX_ID_LEN;

    size_t ch_len = strlen(msg->channel);
    memcpy(buf + offset, msg->channel, ch_len);
    offset += ch_len;

    memcpy(buf + offset, &msg->timestamp_ms, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    size_t cont_len = strlen(msg->content);
    if (offset + cont_len > max_buf) {
        cont_len = max_buf - offset;
    }
    memcpy(buf + offset, msg->content, cont_len);
    offset += cont_len;

    return offset;
}

int wex_chat_msg_sign(wex_chat_msg_t *msg, const wex_identity_t *ident) {
    if (!msg || !ident) {
        return -1;
    }

    memcpy(msg->author_pk, ident->sign_pk, 32);
    strncpy(msg->author, ident->nickname, sizeof(msg->author) - 1);
    msg->author[sizeof(msg->author) - 1] = '\0';

    if (msg->timestamp_ms == 0) {
        msg->timestamp_ms = (uint64_t)time(NULL) * 1000ULL;
    }

    uint8_t payload[WEX_MAX_MSG_LEN + 128];
    size_t payload_len = format_msg_sign_payload(msg, payload, sizeof(payload));

    crypto_ed25519_sign(msg->signature, ident->sign_sk, payload, payload_len);
    return 0;
}

bool wex_chat_msg_verify(const wex_chat_msg_t *msg) {
    if (!msg) {
        return false;
    }

    uint8_t payload[WEX_MAX_MSG_LEN + 128];
    size_t payload_len = format_msg_sign_payload(msg, payload, sizeof(payload));

    int res = crypto_ed25519_check(msg->signature, msg->author_pk, payload, payload_len);
    return (res == 0);
}
