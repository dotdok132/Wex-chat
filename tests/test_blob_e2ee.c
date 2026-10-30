#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include "wex_blob.h"
#include "wex_crypto.h"

#define TEST_PASS(desc) printf("[PASS] %s\n", desc)

int main(void) {
    printf("[*] Running E2EE Blob & Attachment Encryption Tests...\n");

    /* 1. Key generation */
    uint8_t key[32];
    assert(wex_blob_generate_key(key) == 0);
    bool non_zero = false;
    for (int i = 0; i < 32; i++) {
        if (key[i] != 0) non_zero = true;
    }
    assert(non_zero);
    TEST_PASS("wex_blob_generate_key produces secure 256-bit key");

    /* 2. Payload encryption & decryption */
    const char *secret_doc = "Sovereign top secret document with zero KYC and E2EE!";
    size_t doc_len = strlen(secret_doc);

    uint8_t *ciphertext = NULL;
    size_t cipher_len = 0;
    assert(wex_blob_encrypt_payload((const uint8_t *)secret_doc, doc_len, key, &ciphertext, &cipher_len) == 0);
    assert(cipher_len == doc_len + 40); /* 24 nonce + 16 MAC */
    assert(memcmp(ciphertext + 40, secret_doc, doc_len) != 0); /* Obfuscated */

    uint8_t *decrypted = NULL;
    size_t decrypted_len = 0;
    assert(wex_blob_decrypt_payload(ciphertext, cipher_len, key, &decrypted, &decrypted_len) == 0);
    assert(decrypted_len == doc_len);
    assert(memcmp(decrypted, secret_doc, doc_len) == 0);
    free(decrypted);
    TEST_PASS("Round-trip E2EE blob encryption and decryption");

    /* 3. Wrong key rejection */
    uint8_t wrong_key[32];
    assert(wex_blob_generate_key(wrong_key) == 0);
    wrong_key[0] ^= 0xFF;
    assert(wex_blob_decrypt_payload(ciphertext, cipher_len, wrong_key, &decrypted, &decrypted_len) != 0);
    TEST_PASS("Wrong key tamper resistance (Poly1305 MAC failure)");

    /* 4. Ciphertext corruption rejection */
    ciphertext[30] ^= 0x01; /* Corrupt MAC */
    assert(wex_blob_decrypt_payload(ciphertext, cipher_len, key, &decrypted, &decrypted_len) != 0);
    ciphertext[30] ^= 0x01; /* Restore */

    ciphertext[45] ^= 0x42; /* Corrupt payload */
    assert(wex_blob_decrypt_payload(ciphertext, cipher_len, key, &decrypted, &decrypted_len) != 0);
    ciphertext[45] ^= 0x42; /* Restore */
    TEST_PASS("Ciphertext and MAC corruption detection");

    /* 5. Integration with blob chunk storage */
    const char *test_dir = "./.test_blob_e2ee";
    mkdir(test_dir, 0700);

    wex_blob_info_t info;
    assert(wex_blob_info_create(&info, "secret.pdf", "application/pdf", ciphertext, cipher_len) == 0);
    assert(wex_blob_save(test_dir, &info, ciphertext) == 0);

    uint8_t *loaded_cipher = NULL;
    size_t loaded_sz = 0;
    assert(wex_blob_load(test_dir, info.root_hash, &loaded_cipher, &loaded_sz) == 0);
    assert(loaded_sz == cipher_len);
    assert(memcmp(loaded_cipher, ciphertext, cipher_len) == 0);

    /* Decrypt after loading from disk */
    uint8_t *restored_doc = NULL;
    size_t restored_len = 0;
    assert(wex_blob_decrypt_payload(loaded_cipher, loaded_sz, key, &restored_doc, &restored_len) == 0);
    assert(restored_len == doc_len);
    assert(memcmp(restored_doc, secret_doc, doc_len) == 0);

    free(ciphertext);
    free(loaded_cipher);
    free(restored_doc);
    system("rm -rf " "./.test_blob_e2ee");

    TEST_PASS("Disk persistence & atomic retrieval of E2EE blobs");
    printf("[SUCCESS] All E2EE Blob tests passed successfully!\n");
    return 0;
}
