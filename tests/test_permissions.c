#define _GNU_SOURCE
#include "wex_permissions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)

static void test_rbac_roles_and_privileges(void) {
    wex_identity_t owner, alice, bob;
    assert(wex_identity_init(&owner, NULL, "GuildOwner") == 0);
    assert(wex_identity_init(&alice, NULL, "Alice") == 0);
    assert(wex_identity_init(&bob, NULL, "Bob") == 0);

    uint8_t space_id[32] = {0x42};
    wex_acl_t acl;
    wex_acl_init(&acl, space_id, owner.sign_pk);

    /* 1. Owner has full permissions */
    assert(wex_acl_has_permission(&acl, owner.sign_pk, WEX_PERM_ADMINISTRATOR) == true);
    assert(wex_acl_has_permission(&acl, owner.sign_pk, WEX_PERM_BAN_MEMBERS) == true);
    assert(wex_acl_has_permission(&acl, owner.sign_pk, WEX_PERM_SEND_MESSAGES) == true);

    /* 2. Alice as regular member has send/speak permissions, but NOT ban/admin */
    assert(wex_acl_has_permission(&acl, alice.sign_pk, WEX_PERM_SEND_MESSAGES) == true);
    assert(wex_acl_has_permission(&acl, alice.sign_pk, WEX_PERM_BAN_MEMBERS) == false);
    assert(wex_acl_has_permission(&acl, alice.sign_pk, WEX_PERM_ADMINISTRATOR) == false);

    /* 3. Promote Alice to Admin */
    assert(wex_acl_assign_member_role(&acl, alice.sign_pk, "Admin") == 0);
    assert(wex_acl_has_permission(&acl, alice.sign_pk, WEX_PERM_ADMINISTRATOR) == true);
    assert(wex_acl_has_permission(&acl, alice.sign_pk, WEX_PERM_BAN_MEMBERS) == true);

    /* 4. Mute Bob */
    assert(wex_acl_assign_member_role(&acl, bob.sign_pk, "Muted") == 0);
    assert(wex_acl_has_permission(&acl, bob.sign_pk, WEX_PERM_SEND_MESSAGES) == false);

    TEST_PASS("Role-Based Access Control (RBAC) & Privileges Resolution");
}

static void test_cryptographic_bans(void) {
    wex_identity_t owner, spammer, impostor;
    assert(wex_identity_init(&owner, NULL, "GuildOwner") == 0);
    assert(wex_identity_init(&spammer, NULL, "Spammer99") == 0);
    assert(wex_identity_init(&impostor, NULL, "ImpostorEve") == 0);

    uint8_t space_id[32] = {0xAA};
    wex_acl_t acl;
    wex_acl_init(&acl, space_id, owner.sign_pk);

    assert(wex_acl_is_banned(&acl, spammer.sign_pk) == false);

    /* Impostor tries to forge a ban against Spammer */
    wex_ban_entry_t forged_ban;
    assert(wex_acl_ban_create(&impostor, space_id, spammer.sign_pk, "Fake ban", &forged_ban) == 0);
    assert(wex_acl_apply_ban(&acl, &forged_ban) == -2); /* Signature check must fail! */
    assert(wex_acl_is_banned(&acl, spammer.sign_pk) == false);

    /* Legitimate Space Owner issues signed ban */
    wex_ban_entry_t legit_ban;
    assert(wex_acl_ban_create(&owner, space_id, spammer.sign_pk, "Flooding network with spam", &legit_ban) == 0);
    assert(wex_acl_apply_ban(&acl, &legit_ban) == 0);

    /* Spammer is now banned and stripped of all permissions */
    assert(wex_acl_is_banned(&acl, spammer.sign_pk) == true);
    assert(wex_acl_has_permission(&acl, spammer.sign_pk, WEX_PERM_SEND_MESSAGES) == false);
    assert(wex_acl_has_permission(&acl, spammer.sign_pk, WEX_PERM_VOICE_CONNECT) == false);

    TEST_PASS("Ed25519 Cryptographically Signed Space Ban & Forgery Defense");
}

int main(void) {
    printf("====================================================\n");
    printf("  Running Wex Permissions & Moderation Tests        \n");
    printf("====================================================\n");

    test_rbac_roles_and_privileges();
    test_cryptographic_bans();

    printf("====================================================\n");
    printf("  ALL PERMISSIONS TESTS PASSED SUCCESSFULLY!        \n");
    printf("====================================================\n");
    return 0;
}
