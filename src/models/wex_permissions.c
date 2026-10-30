#define _GNU_SOURCE
#include "wex_permissions.h"
#include "../../vendor/monocypher/monocypher.h"
#include "../../vendor/monocypher/monocypher-ed25519.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void wex_acl_init(wex_acl_t *acl, const uint8_t space_id[32], const uint8_t owner_pk[32]) {
    if (!acl) return;
    memset(acl, 0, sizeof(*acl));
    if (space_id) memcpy(acl->space_id, space_id, 32);
    if (owner_pk) memcpy(acl->owner_pk, owner_pk, 32);

    /* Built-in default roles */
    wex_acl_add_role(acl, "Admin", WEX_PERM_ADMINISTRATOR);
    wex_acl_add_role(acl, "Member", WEX_PERM_SEND_MESSAGES | WEX_PERM_ATTACH_FILES |
                                    WEX_PERM_VOICE_CONNECT | WEX_PERM_VOICE_SPEAK);
    wex_acl_add_role(acl, "Muted", 0);
}

int wex_acl_add_role(wex_acl_t *acl, const char *role_name, uint32_t permissions) {
    if (!acl || !role_name) return -1;

    for (size_t i = 0; i < acl->role_count; i++) {
        if (strcmp(acl->roles[i].name, role_name) == 0) {
            acl->roles[i].permissions = permissions;
            return 0;
        }
    }

    if (acl->role_count < WEX_MAX_ROLES) {
        wex_role_t *r = &acl->roles[acl->role_count++];
        strncpy(r->name, role_name, sizeof(r->name) - 1);
        r->permissions = permissions;
        return 0;
    }

    return -1;
}

int wex_acl_assign_member_role(wex_acl_t *acl, const uint8_t peer_pk[32], const char *role_name) {
    if (!acl || !peer_pk || !role_name) return -1;

    uint32_t perms = 0;
    bool found_role = false;
    for (size_t i = 0; i < acl->role_count; i++) {
        if (strcmp(acl->roles[i].name, role_name) == 0) {
            perms = acl->roles[i].permissions;
            found_role = true;
            break;
        }
    }
    if (!found_role) return -1;

    for (size_t i = 0; i < acl->member_count; i++) {
        if (memcmp(acl->members[i].peer_pk, peer_pk, 32) == 0) {
            acl->members[i].permissions = perms;
            strncpy(acl->members[i].role_name, role_name, sizeof(acl->members[i].role_name) - 1);
            return 0;
        }
    }

    if (acl->member_count < WEX_MAX_MEMBER_ROLES) {
        wex_member_perm_t *m = &acl->members[acl->member_count++];
        memcpy(m->peer_pk, peer_pk, 32);
        m->permissions = perms;
        strncpy(m->role_name, role_name, sizeof(m->role_name) - 1);
        return 0;
    }

    return -1;
}

bool wex_acl_is_banned(const wex_acl_t *acl, const uint8_t peer_pk[32]) {
    if (!acl || !peer_pk) return false;
    for (size_t i = 0; i < acl->ban_count; i++) {
        if (memcmp(acl->bans[i].peer_pk, peer_pk, 32) == 0) {
            return true;
        }
    }
    return false;
}

bool wex_acl_has_permission(const wex_acl_t *acl, const uint8_t peer_pk[32], uint32_t permission_flag) {
    if (!acl || !peer_pk) return false;

    /* Space Owner has unconditional privileges */
    if (memcmp(peer_pk, acl->owner_pk, 32) == 0) {
        return true;
    }

    /* Banned peers have zero permissions */
    if (wex_acl_is_banned(acl, peer_pk)) {
        return false;
    }

    /* Check explicit member assignment */
    for (size_t i = 0; i < acl->member_count; i++) {
        if (memcmp(acl->members[i].peer_pk, peer_pk, 32) == 0) {
            uint32_t p = acl->members[i].permissions;
            if (p & WEX_PERM_ADMINISTRATOR) return true;
            return (p & permission_flag) != 0;
        }
    }

    /* Default permissions for regular members without explicit role */
    uint32_t default_perms = WEX_PERM_SEND_MESSAGES | WEX_PERM_ATTACH_FILES |
                             WEX_PERM_VOICE_CONNECT | WEX_PERM_VOICE_SPEAK;
    return (default_perms & permission_flag) != 0;
}

static void compute_ban_hash(const wex_ban_entry_t *ban, uint8_t out_hash[32]) {
    uint8_t buffer[32 + 32 + 8 + 128];
    size_t off = 0;
    memcpy(buffer + off, ban->space_id, 32); off += 32;
    memcpy(buffer + off, ban->peer_pk, 32); off += 32;
    memcpy(buffer + off, &ban->timestamp_ms, 8); off += 8;
    memcpy(buffer + off, ban->reason, 128); off += 128;

    crypto_blake2b(out_hash, 32, buffer, off);
}

int wex_acl_ban_create(const wex_identity_t *owner_ident, const uint8_t space_id[32],
                       const uint8_t target_pk[32], const char *reason, wex_ban_entry_t *out_ban) {
    if (!owner_ident || !space_id || !target_pk || !out_ban) return -1;

    memset(out_ban, 0, sizeof(*out_ban));
    memcpy(out_ban->space_id, space_id, 32);
    memcpy(out_ban->peer_pk, target_pk, 32);
    if (reason) {
        strncpy(out_ban->reason, reason, sizeof(out_ban->reason) - 1);
    } else {
        strncpy(out_ban->reason, "Violated community rules", sizeof(out_ban->reason) - 1);
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    out_ban->timestamp_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;

    uint8_t hash[32];
    compute_ban_hash(out_ban, hash);

    crypto_ed25519_sign(out_ban->signature, owner_ident->sign_sk, hash, sizeof(hash));
    return 0;
}

int wex_acl_apply_ban(wex_acl_t *acl, const wex_ban_entry_t *ban) {
    if (!acl || !ban) return -1;
    if (memcmp(acl->space_id, ban->space_id, 32) != 0) return -1;

    uint8_t hash[32];
    compute_ban_hash(ban, hash);

    /* Verify signature against Space Owner public key */
    if (crypto_ed25519_check(ban->signature, acl->owner_pk, hash, sizeof(hash)) != 0) {
        return -2; /* Forged ban signature */
    }

    /* Check if already in ban list */
    for (size_t i = 0; i < acl->ban_count; i++) {
        if (memcmp(acl->bans[i].peer_pk, ban->peer_pk, 32) == 0) {
            return 0; /* Already banned */
        }
    }

    if (acl->ban_count < WEX_MAX_BANS) {
        acl->bans[acl->ban_count++] = *ban;

        /* Strip all assigned permissions */
        for (size_t i = 0; i < acl->member_count; i++) {
            if (memcmp(acl->members[i].peer_pk, ban->peer_pk, 32) == 0) {
                acl->members[i] = acl->members[acl->member_count - 1];
                acl->member_count--;
                break;
            }
        }
        return 0;
    }

    return -1;
}
