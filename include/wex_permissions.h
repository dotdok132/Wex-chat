#ifndef WEX_PERMISSIONS_H
#define WEX_PERMISSIONS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "wex_models.h"

/* Permission bitflags */
#define WEX_PERM_SEND_MESSAGES   (1U << 0)
#define WEX_PERM_ATTACH_FILES    (1U << 1)
#define WEX_PERM_MANAGE_CHANNELS (1U << 2)
#define WEX_PERM_KICK_MEMBERS    (1U << 3)
#define WEX_PERM_BAN_MEMBERS     (1U << 4)
#define WEX_PERM_VOICE_CONNECT   (1U << 5)
#define WEX_PERM_VOICE_SPEAK     (1U << 6)
#define WEX_PERM_ADMINISTRATOR   (1U << 31)

#define WEX_MAX_ROLES 8
#define WEX_MAX_MEMBER_ROLES 64
#define WEX_MAX_BANS 64

typedef struct {
    char name[32];
    uint32_t permissions;
} wex_role_t;

typedef struct {
    uint8_t peer_pk[32];
    uint32_t permissions;
    char role_name[32];
} wex_member_perm_t;

typedef struct {
    uint8_t peer_pk[32];
    uint8_t space_id[32];
    char reason[128];
    uint64_t timestamp_ms;
    uint8_t signature[64]; /* Signed by Space Owner */
} wex_ban_entry_t;

typedef struct {
    uint8_t space_id[32];
    uint8_t owner_pk[32];
    
    /* Configured roles */
    wex_role_t roles[WEX_MAX_ROLES];
    size_t role_count;

    /* Member permission assignments */
    wex_member_perm_t members[WEX_MAX_MEMBER_ROLES];
    size_t member_count;

    /* Blacklist / Bans */
    wex_ban_entry_t bans[WEX_MAX_BANS];
    size_t ban_count;
} wex_acl_t;

/* Initialize ACL for a space */
void wex_acl_init(wex_acl_t *acl, const uint8_t space_id[32], const uint8_t owner_pk[32]);

/* Add or configure a role */
int wex_acl_add_role(wex_acl_t *acl, const char *role_name, uint32_t permissions);

/* Assign role/permissions to a peer */
int wex_acl_assign_member_role(wex_acl_t *acl, const uint8_t peer_pk[32], const char *role_name);

/* Check if a peer has a specific permission */
bool wex_acl_has_permission(const wex_acl_t *acl, const uint8_t peer_pk[32], uint32_t permission_flag);

/* Space owner signs a ban entry */
int wex_acl_ban_create(const wex_identity_t *owner_ident, const uint8_t space_id[32],
                       const uint8_t target_pk[32], const char *reason, wex_ban_entry_t *out_ban);

/* Verify authenticity of a ban entry and apply it to space ACL */
int wex_acl_apply_ban(wex_acl_t *acl, const wex_ban_entry_t *ban);

/* Check if a peer is banned from the space */
bool wex_acl_is_banned(const wex_acl_t *acl, const uint8_t peer_pk[32]);

#endif /* WEX_PERMISSIONS_H */
