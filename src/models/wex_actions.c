#define _GNU_SOURCE
#include "wex_actions.h"
#include "../../vendor/monocypher/monocypher.h"
#include "../../vendor/monocypher/monocypher-ed25519.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void compute_action_hash(const wex_msg_action_t *action, uint8_t out_hash[32]) {
    uint8_t buffer[1 + 64 + 32 + WEX_NAME_LEN + WEX_NAME_LEN + 32 + 8 + 512];
    size_t off = 0;

    buffer[off++] = action->action_type;
    memcpy(buffer + off, action->target_sig, 64); off += 64;
    memcpy(buffer + off, action->space_id, 32); off += 32;
    memcpy(buffer + off, action->channel, WEX_NAME_LEN); off += WEX_NAME_LEN;
    memcpy(buffer + off, action->actor, WEX_NAME_LEN); off += WEX_NAME_LEN;
    memcpy(buffer + off, action->actor_pk, 32); off += 32;
    memcpy(buffer + off, &action->timestamp_ms, 8); off += 8;
    memcpy(buffer + off, action->payload, 512); off += 512;

    crypto_blake2b(out_hash, 32, buffer, off);
}

int wex_msg_action_sign(wex_msg_action_t *action, const wex_identity_t *ident) {
    if (!action || !ident) return -1;

    memcpy(action->actor_pk, ident->sign_pk, 32);
    if (strlen(action->actor) == 0) {
        strncpy(action->actor, ident->nickname, sizeof(action->actor) - 1);
    }

    uint8_t hash[32];
    compute_action_hash(action, hash);

    crypto_ed25519_sign(action->action_sig, ident->sign_sk, hash, sizeof(hash));
    return 0;
}

bool wex_msg_action_verify(const wex_msg_action_t *action) {
    if (!action) return false;

    uint8_t hash[32];
    compute_action_hash(action, hash);

    return (crypto_ed25519_check(action->action_sig, action->actor_pk, hash, sizeof(hash)) == 0);
}

void wex_reactions_store_init(wex_reactions_store_t *store) {
    if (!store) return;
    memset(store, 0, sizeof(*store));
}

int wex_msg_action_apply(wex_reactions_store_t *store, const wex_acl_t *acl,
                         wex_chat_msg_t *msg, const wex_msg_action_t *action) {
    if (!msg || !action) return -1;

    /* 1. Cryptographic integrity check */
    if (!wex_msg_action_verify(action)) {
        return -1; /* Forged or invalid action signature */
    }

    /* 2. Target signature match */
    if (memcmp(msg->signature, action->target_sig, 64) != 0) {
        return -2; /* Target signature mismatch */
    }

    /* 3. Check banned actor */
    if (acl && wex_acl_is_banned(acl, action->actor_pk)) {
        return -3; /* Banned actor cannot perform actions */
    }

    switch (action->action_type) {
        case WEX_ACTION_EDIT: {
            /* Only original author can edit their own message */
            if (memcmp(action->actor_pk, msg->author_pk, 32) != 0) {
                return -4; /* Permission denied: not message author */
            }
            strncpy(msg->content, action->payload, sizeof(msg->content) - 1);
            msg->content[sizeof(msg->content) - 1] = '\0';
            return 0;
        }

        case WEX_ACTION_DELETE: {
            /* Message author OR space admin can delete */
            bool is_author = (memcmp(action->actor_pk, msg->author_pk, 32) == 0);
            bool is_admin = (acl != NULL && wex_acl_has_permission(acl, action->actor_pk, WEX_PERM_ADMINISTRATOR));

            if (!is_author && !is_admin) {
                return -5; /* Permission denied: not author or admin */
            }

            strncpy(msg->content, "[Message deleted]", sizeof(msg->content) - 1);
            msg->content[sizeof(msg->content) - 1] = '\0';
            return 0;
        }

        case WEX_ACTION_REACT: {
            if (!store || strlen(action->payload) == 0) return -6;

            /* Find or allocate tracked entry for target message */
            wex_msg_reactions_t *entry = NULL;
            for (size_t i = 0; i < store->tracked_count; i++) {
                if (memcmp(store->tracked[i].target_sig, action->target_sig, 64) == 0) {
                    entry = &store->tracked[i];
                    break;
                }
            }

            if (!entry && store->tracked_count < WEX_MAX_TRACKED_ACTIONS) {
                entry = &store->tracked[store->tracked_count++];
                memcpy(entry->target_sig, action->target_sig, 64);
            }
            if (!entry) return -7;

            /* Find emoji reaction */
            wex_reaction_t *r = NULL;
            for (size_t i = 0; i < entry->reaction_count; i++) {
                if (strcmp(entry->reactions[i].emoji, action->payload) == 0) {
                    r = &entry->reactions[i];
                    break;
                }
            }

            if (!r && entry->reaction_count < WEX_MAX_REACTIONS_PER_MSG) {
                r = &entry->reactions[entry->reaction_count++];
                strncpy(r->emoji, action->payload, sizeof(r->emoji) - 1);
                r->count = 0;
            }
            if (!r) return -8;

            /* Toggle reaction: if already reacted, remove; else add */
            for (size_t u = 0; u < r->count; u++) {
                if (memcmp(r->user_pks[u], action->actor_pk, 32) == 0) {
                    /* Remove reaction */
                    memcpy(r->user_pks[u], r->user_pks[r->count - 1], 32);
                    r->count--;
                    return 0;
                }
            }

            if (r->count < 16) {
                memcpy(r->user_pks[r->count++], action->actor_pk, 32);
                return 0;
            }
            return 0;
        }

        default:
            return -9;
    }
}

int wex_reactions_get_summary(const wex_reactions_store_t *store, const uint8_t target_sig[64],
                              char *out_buf, size_t out_len) {
    if (!store || !target_sig || !out_buf || out_len == 0) return -1;
    out_buf[0] = '\0';

    for (size_t i = 0; i < store->tracked_count; i++) {
        if (memcmp(store->tracked[i].target_sig, target_sig, 64) == 0) {
            const wex_msg_reactions_t *entry = &store->tracked[i];
            size_t off = 0;
            for (size_t r = 0; r < entry->reaction_count; r++) {
                if (entry->reactions[r].count > 0) {
                    int w = snprintf(out_buf + off, out_len - off, "%s %zu  ",
                                     entry->reactions[r].emoji, entry->reactions[r].count);
                    if (w > 0) off += (size_t)w;
                    if (off >= out_len) break;
                }
            }
            return 0;
        }
    }
    return 0;
}
