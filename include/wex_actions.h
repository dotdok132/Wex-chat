#ifndef WEX_ACTIONS_H
#define WEX_ACTIONS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "wex_models.h"
#include "wex_permissions.h"

typedef enum {
    WEX_ACTION_REACT  = 0x01, /* Add or toggle emoji reaction */
    WEX_ACTION_EDIT   = 0x02, /* Edit message content (author only) */
    WEX_ACTION_DELETE = 0x03  /* Retract/delete message (author or space admin) */
} wex_msg_action_type_t;

#define WEX_MAX_REACTIONS_PER_MSG 16
#define WEX_MAX_TRACKED_ACTIONS 128

#pragma pack(push, 1)
typedef struct {
    uint8_t  action_type;       /* wex_msg_action_type_t */
    uint8_t  target_sig[64];    /* Signature of message being modified */
    uint8_t  space_id[32];      /* Target space ID */
    char     channel[WEX_NAME_LEN];
    char     actor[WEX_NAME_LEN];
    uint8_t  actor_pk[32];
    uint64_t timestamp_ms;
    char     payload[512];      /* Emoji (e.g. "👍") or new edited text */
    uint8_t  action_sig[64];    /* Ed25519 signature over action body */
} wex_msg_action_t;
#pragma pack(pop)

typedef struct {
    char emoji[16];
    size_t count;
    uint8_t user_pks[16][32];
} wex_reaction_t;

typedef struct {
    uint8_t target_sig[64];
    wex_reaction_t reactions[WEX_MAX_REACTIONS_PER_MSG];
    size_t reaction_count;
} wex_msg_reactions_t;

typedef struct {
    wex_msg_reactions_t tracked[WEX_MAX_TRACKED_ACTIONS];
    size_t tracked_count;
} wex_reactions_store_t;

/* Sign a message action */
int wex_msg_action_sign(wex_msg_action_t *action, const wex_identity_t *ident);

/* Verify authenticity and integrity of a message action */
bool wex_msg_action_verify(const wex_msg_action_t *action);

/* Initialize reactions store */
void wex_reactions_store_init(wex_reactions_store_t *store);

/* Apply an action to a chat message; returns 0 on success, <0 on permission error or invalid action */
int wex_msg_action_apply(wex_reactions_store_t *store, const wex_acl_t *acl,
                         wex_chat_msg_t *msg, const wex_msg_action_t *action);

/* Get formatted reactions string for a message: "👍 3  🔥 1" */
int wex_reactions_get_summary(const wex_reactions_store_t *store, const uint8_t target_sig[64],
                              char *out_buf, size_t out_len);

#endif /* WEX_ACTIONS_H */
