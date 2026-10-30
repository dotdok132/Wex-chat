#define _GNU_SOURCE
#include "wex_blob.h"
#include "wex_persist.h"
#include "wex_crypto.h"
#include "../../vendor/monocypher/monocypher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

int wex_blob_compute_hash(const uint8_t *data, size_t size, uint8_t out_hash[WEX_BLOB_HASH_LEN]) {
    if (!out_hash || (size > 0 && !data)) return -1;
    crypto_blake2b(out_hash, WEX_BLOB_HASH_LEN, data, size);
    return 0;
}

int wex_blob_info_create(wex_blob_info_t *info, const char *filename,
                         const char *mime_type, const uint8_t *data, size_t size) {
    if (!info || (size > 0 && !data)) return -1;
    memset(info, 0, sizeof(*info));

    if (filename) {
        strncpy(info->filename, filename, sizeof(info->filename) - 1);
    } else {
        strncpy(info->filename, "file.bin", sizeof(info->filename) - 1);
    }

    if (mime_type) {
        strncpy(info->mime_type, mime_type, sizeof(info->mime_type) - 1);
    } else {
        strncpy(info->mime_type, "application/octet-stream", sizeof(info->mime_type) - 1);
    }

    info->total_size = (uint64_t)size;
    if (size == 0) {
        info->total_chunks = 1;
    } else {
        info->total_chunks = (uint32_t)((size + WEX_BLOB_CHUNK_SIZE - 1) / WEX_BLOB_CHUNK_SIZE);
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    info->created_at_ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;

    return wex_blob_compute_hash(data, size, info->root_hash);
}

int wex_blob_assembler_init(wex_blob_assembler_t *asmbler, const wex_blob_info_t *info) {
    if (!asmbler || !info || info->total_chunks == 0 || info->total_chunks > WEX_BLOB_MAX_CHUNKS) {
        return -1;
    }
    memset(asmbler, 0, sizeof(*asmbler));
    asmbler->info = *info;

    size_t alloc_sz = (info->total_size > 0) ? (size_t)info->total_size : 1;
    asmbler->data = calloc(1, alloc_sz);
    if (!asmbler->data) return -1;

    asmbler->chunk_received = calloc(info->total_chunks, 1);
    if (!asmbler->chunk_received) {
        free(asmbler->data);
        asmbler->data = NULL;
        return -1;
    }

    asmbler->received_count = 0;
    asmbler->completed = false;
    return 0;
}

bool wex_blob_assembler_put_chunk(wex_blob_assembler_t *asmbler, uint32_t chunk_idx,
                                  const uint8_t *chunk_data, size_t chunk_len) {
    if (!asmbler || !asmbler->data || !asmbler->chunk_received || !chunk_data) return false;
    if (chunk_idx >= asmbler->info.total_chunks) return false;

    size_t offset = (size_t)chunk_idx * WEX_BLOB_CHUNK_SIZE;
    if (offset + chunk_len > asmbler->info.total_size && asmbler->info.total_size > 0) {
        return false;
    }

    if (!asmbler->chunk_received[chunk_idx]) {
        memcpy(asmbler->data + offset, chunk_data, chunk_len);
        asmbler->chunk_received[chunk_idx] = 1;
        asmbler->received_count++;
    }

    if (asmbler->received_count == asmbler->info.total_chunks) {
        /* Verify full integrity against root hash */
        uint8_t computed[WEX_BLOB_HASH_LEN];
        wex_blob_compute_hash(asmbler->data, (size_t)asmbler->info.total_size, computed);
        if (crypto_verify32(computed, asmbler->info.root_hash) == 0) {
            asmbler->completed = true;
            return true;
        }
    }

    return false;
}

void wex_blob_assembler_free(wex_blob_assembler_t *asmbler) {
    if (!asmbler) return;
    if (asmbler->data) {
        free(asmbler->data);
        asmbler->data = NULL;
    }
    if (asmbler->chunk_received) {
        free(asmbler->chunk_received);
        asmbler->chunk_received = NULL;
    }
    asmbler->received_count = 0;
    asmbler->completed = false;
}

static void hash_to_hex(const uint8_t hash[32], char hex[65]) {
    for (int i = 0; i < 32; i++) {
        sprintf(hex + (i * 2), "%02x", hash[i]);
    }
    hex[64] = '\0';
}

int wex_blob_save(const char *storage_dir, const wex_blob_info_t *info, const uint8_t *data) {
    if (!storage_dir || !info || (info->total_size > 0 && !data)) return -1;

    char blobs_dir[512];
    snprintf(blobs_dir, sizeof(blobs_dir), "%s/blobs", storage_dir);
    wex_mkdir_p(blobs_dir);

    char hex[65];
    hash_to_hex(info->root_hash, hex);

    char filepath[600];
    snprintf(filepath, sizeof(filepath), "%s/%s", blobs_dir, hex);

    char tmp_path[620];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", filepath, (int)getpid());

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    if (info->total_size > 0) {
        ssize_t w = write(fd, data, (size_t)info->total_size);
        if (w != (ssize_t)info->total_size) {
            close(fd);
            unlink(tmp_path);
            return -1;
        }
    }

    fsync(fd);
    close(fd);

    if (rename(tmp_path, filepath) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

int wex_blob_load(const char *storage_dir, const uint8_t root_hash[WEX_BLOB_HASH_LEN],
                  uint8_t **out_data, size_t *out_size) {
    if (!storage_dir || !root_hash || !out_data || !out_size) return -1;

    char hex[65];
    hash_to_hex(root_hash, hex);

    char filepath[600];
    snprintf(filepath, sizeof(filepath), "%s/blobs/%s", storage_dir, hex);

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }

    size_t sz = (size_t)st.st_size;
    uint8_t *buf = malloc(sz > 0 ? sz : 1);
    if (!buf) {
        close(fd);
        return -1;
    }

    if (sz > 0) {
        ssize_t r = read(fd, buf, sz);
        if (r != (ssize_t)sz) {
            free(buf);
            close(fd);
            return -1;
        }
    }
    close(fd);

    /* Verify integrity */
    uint8_t computed[WEX_BLOB_HASH_LEN];
    wex_blob_compute_hash(buf, sz, computed);
    if (crypto_verify32(computed, root_hash) != 0) {
        free(buf);
        return -2; /* Hash mismatch / corrupted blob */
    }

    *out_data = buf;
    *out_size = sz;
    return 0;
}

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *wex_b64_encode(const uint8_t *data, size_t input_len) {
    if (!data && input_len > 0) return NULL;
    size_t out_len = 4 * ((input_len + 2) / 3);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < input_len) {
        size_t rem = input_len - i;
        uint32_t a = data[i++];
        uint32_t b = (rem > 1) ? data[i++] : 0;
        uint32_t c = (rem > 2) ? data[i++] : 0;

        uint32_t triple = (a << 16) | (b << 8) | c;

        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (rem > 1) ? b64_table[(triple >> 6) & 0x3F] : '=';
        out[j++] = (rem > 2) ? b64_table[triple & 0x3F] : '=';
    }
    out[j] = '\0';
    return out;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

uint8_t *wex_b64_decode(const char *b64_str, size_t *out_len) {
    if (!b64_str || !out_len) return NULL;
    size_t len = strlen(b64_str);
    if (len % 4 != 0) return NULL;

    size_t alloc_sz = (len / 4) * 3;
    uint8_t *out = malloc(alloc_sz + 1);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i += 4) {
        int v0 = b64_val(b64_str[i]);
        int v1 = b64_val(b64_str[i + 1]);
        int v2 = (b64_str[i + 2] == '=') ? 0 : b64_val(b64_str[i + 2]);
        int v3 = (b64_str[i + 3] == '=') ? 0 : b64_val(b64_str[i + 3]);

        if (v0 < 0 || v1 < 0 || (b64_str[i + 2] != '=' && v2 < 0) || (b64_str[i + 3] != '=' && v3 < 0)) {
            free(out);
            return NULL;
        }

        uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;

        out[j++] = (uint8_t)((triple >> 16) & 0xFF);
        if (b64_str[i + 2] != '=') {
            out[j++] = (uint8_t)((triple >> 8) & 0xFF);
        }
        if (b64_str[i + 3] != '=') {
            out[j++] = (uint8_t)(triple & 0xFF);
        }
    }

    *out_len = j;
    return out;
}

int wex_blob_generate_key(uint8_t out_key[32]) {
    if (!out_key) return -1;
    return wex_randombytes(out_key, 32);
}

int wex_blob_encrypt_payload(const uint8_t *plaintext, size_t plain_len,
                             const uint8_t key[32],
                             uint8_t **out_ciphertext, size_t *out_cipher_len) {
    if (!key || (plain_len > 0 && !plaintext) || !out_ciphertext || !out_cipher_len) {
        return -1;
    }

    size_t total_sz = 24 + 16 + plain_len;
    uint8_t *buf = malloc(total_sz ? total_sz : 1);
    if (!buf) return -1;

    uint8_t *nonce = buf;
    uint8_t *mac = buf + 24;
    uint8_t *cipher = buf + 40;

    if (wex_randombytes(nonce, 24) != 0) {
        free(buf);
        return -1;
    }

    crypto_aead_lock(cipher, mac, key, nonce, NULL, 0, plaintext, plain_len);

    *out_ciphertext = buf;
    *out_cipher_len = total_sz;
    return 0;
}

int wex_blob_decrypt_payload(const uint8_t *ciphertext, size_t cipher_len,
                             const uint8_t key[32],
                             uint8_t **out_plaintext, size_t *out_plain_len) {
    if (!key || !ciphertext || cipher_len < 40 || !out_plaintext || !out_plain_len) {
        return -1;
    }

    size_t plain_len = cipher_len - 40;
    uint8_t *plain = malloc(plain_len ? plain_len : 1);
    if (!plain) return -1;

    const uint8_t *nonce = ciphertext;
    const uint8_t *mac = ciphertext + 24;
    const uint8_t *cipher = ciphertext + 40;

    if (crypto_aead_unlock(plain, mac, key, nonce, NULL, 0, cipher, plain_len) != 0) {
        wex_clean(plain, plain_len);
        free(plain);
        *out_plaintext = NULL;
        *out_plain_len = 0;
        return -1; /* Forgery or wrong key */
    }

    *out_plaintext = plain;
    *out_plain_len = plain_len;
    return 0;
}
