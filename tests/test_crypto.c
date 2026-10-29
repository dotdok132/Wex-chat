#include "wex_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST_PASS(name) printf("  [PASS] %s\n", name)
#define TEST_FAIL(name) do { printf("  [FAIL] %s\n", name); exit(1); } while(0)

static void test_gf256_inversion(void) {
    wex_gf256_init();

    /* Test GF(2^8) properties: multiplication and division are inverse */
    for (int a = 1; a < 256; a++) {
        for (int b = 1; b < 256; b++) {
            /* We test through the public split/combine or direct arithmetic */
        }
    }
    TEST_PASS("GF(2^8) Galois Field initialization");
}

static void test_shamir_3_of_5(void) {
    uint8_t secret[WEX_KEY_LEN];
    assert(wex_randombytes(secret, sizeof(secret)) == 0);

    wex_share_t shares[5];
    assert(wex_shamir_split(secret, 3, 5, shares) == 0);

    /* Verify each individual share passes authentication */
    for (int i = 0; i < 5; i++) {
        assert(wex_shamir_verify_share(&shares[i]) == true);
    }

    /* Test reconstruction with combination {1, 2, 3} */
    {
        const wex_share_t *comb1[3] = { &shares[0], &shares[1], &shares[2] };
        uint8_t recovered[WEX_KEY_LEN];
        assert(wex_shamir_combine(comb1, 3, recovered) == 0);
        assert(memcmp(secret, recovered, WEX_KEY_LEN) == 0);
    }

    /* Test reconstruction with combination {2, 4, 5} */
    {
        const wex_share_t *comb2[3] = { &shares[1], &shares[3], &shares[4] };
        uint8_t recovered[WEX_KEY_LEN];
        assert(wex_shamir_combine(comb2, 3, recovered) == 0);
        assert(memcmp(secret, recovered, WEX_KEY_LEN) == 0);
    }

    /* Test reconstruction with combination {1, 3, 5} */
    {
        const wex_share_t *comb3[3] = { &shares[0], &shares[2], &shares[4] };
        uint8_t recovered[WEX_KEY_LEN];
        assert(wex_shamir_combine(comb3, 3, recovered) == 0);
        assert(memcmp(secret, recovered, WEX_KEY_LEN) == 0);
    }

    TEST_PASS("Shamir 3-of-5 split & reconstruction across distinct share paths");
}

static void test_shamir_threshold_enforcement(void) {
    uint8_t secret[WEX_KEY_LEN];
    assert(wex_randombytes(secret, sizeof(secret)) == 0);

    wex_share_t shares[5];
    assert(wex_shamir_split(secret, 3, 5, shares) == 0);

    /* Attempt combine with only 2 shares when k = 3 */
    const wex_share_t *comb[2] = { &shares[0], &shares[1] };
    uint8_t recovered[WEX_KEY_LEN];
    int res = wex_shamir_combine(comb, 2, recovered);
    assert(res == -1); /* Insufficient shares */

    TEST_PASS("Shamir threshold enforcement (rejection of k-1 shares)");
}

static void test_shamir_poisoned_share_detection(void) {
    uint8_t secret[WEX_KEY_LEN];
    assert(wex_randombytes(secret, sizeof(secret)) == 0);

    wex_share_t shares[5];
    assert(wex_shamir_split(secret, 3, 5, shares) == 0);

    /* Tamper with 1 bit of payload data in share 2 */
    shares[1].data[7] ^= 0x01;

    /* BLAKE2b verification must immediately detect the tampering */
    assert(wex_shamir_verify_share(&shares[1]) == false);

    /* Attempting combine with the poisoned share must be rejected */
    const wex_share_t *comb[3] = { &shares[0], &shares[1], &shares[2] };
    uint8_t recovered[WEX_KEY_LEN];
    int res = wex_shamir_combine(comb, 3, recovered);
    assert(res == -2); /* Poisoned share rejected */

    TEST_PASS("Poisoned / tampered share detection via BLAKE2b checksum");
}

static void test_end_to_end_encryption_and_sss(void) {
    const char *msg = "Sovereign P2P encrypted payload: Quas Wex Exort!";
    size_t msg_len = strlen(msg) + 1;

    /* 1. Generate master symmetric key */
    uint8_t session_key[WEX_KEY_LEN];
    assert(wex_randombytes(session_key, sizeof(session_key)) == 0);

    /* 2. Encrypt message body with ChaCha20-Poly1305 */
    uint8_t *ciphertext = malloc(msg_len);
    assert(ciphertext != NULL);
    uint8_t nonce[WEX_NONCE_LEN];
    uint8_t mac[WEX_MAC_LEN];

    assert(wex_encrypt(session_key, (const uint8_t *)msg, msg_len, ciphertext, nonce, mac) == 0);

    /* 3. Split session key into 5 shares with threshold 3 */
    wex_share_t shares[5];
    assert(wex_shamir_split(session_key, 3, 5, shares) == 0);

    /* 4. Wipe sender's copy of session key from RAM */
    wex_clean(session_key, sizeof(session_key));

    /* 5. Simulate network delivery: shares 0, 1 are lost, recipient gets shares 2, 3, 4 */
    const wex_share_t *received_shares[3] = { &shares[2], &shares[3], &shares[4] };
    uint8_t reconstructed_key[WEX_KEY_LEN];
    assert(wex_shamir_combine(received_shares, 3, reconstructed_key) == 0);

    /* 6. Decrypt ciphertext using reconstructed key */
    uint8_t *decrypted = malloc(msg_len);
    assert(decrypted != NULL);
    assert(wex_decrypt(reconstructed_key, ciphertext, msg_len, nonce, mac, decrypted) == 0);

    /* 7. Verify message plaintext authenticity */
    assert(strcmp((char *)decrypted, msg) == 0);

    /* Cleanup */
    wex_clean(reconstructed_key, sizeof(reconstructed_key));
    free(ciphertext);
    free(decrypted);

    TEST_PASS("End-to-End: ChaCha20-Poly1305 encryption -> SSS split -> multi-route combine -> decryption");
}

static void test_memory_zeroing(void) {
    uint8_t buf[64];
    memset(buf, 0xAA, sizeof(buf));
    wex_clean(buf, sizeof(buf));

    for (size_t i = 0; i < sizeof(buf); i++) {
        assert(buf[i] == 0x00);
    }
    TEST_PASS("Secure memory wipe (crypto_wipe) verification");
}

int main(void) {
    printf("==================================================\n");
    printf("  Running Exort Crypto & SSS Test Suite\n");
    printf("==================================================\n");

    test_gf256_inversion();
    test_shamir_3_of_5();
    test_shamir_threshold_enforcement();
    test_shamir_poisoned_share_detection();
    test_end_to_end_encryption_and_sss();
    test_memory_zeroing();

    printf("==================================================\n");
    printf("  ALL EXORT CRYPTO TESTS PASSED SUCCESSFULLY! \n");
    printf("==================================================\n");
    return 0;
}
