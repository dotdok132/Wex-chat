#include "wex_crypto.h"
#include "../../vendor/monocypher/monocypher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(__linux__)
#include <sys/random.h>
#endif

/* Galois Field GF(2^8) lookup tables */
static uint8_t exp_tbl[512];
static uint8_t log_tbl[256];
static int gf_initialized = 0;

void wex_clean(void *buf, size_t len) {
    if (buf && len > 0) {
        crypto_wipe(buf, len);
    }
}

int wex_randombytes(void *buf, size_t len) {
    if (!buf || len == 0) {
        return -1;
    }

#if defined(__linux__)
    ssize_t res = getrandom(buf, len, 0);
    if (res == (ssize_t)len) {
        return 0;
    }
#endif

    /* Fallback to /dev/urandom */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    size_t total_read = 0;
    while (total_read < len) {
        ssize_t n = read(fd, (uint8_t *)buf + total_read, len - total_read);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        total_read += (size_t)n;
    }

    close(fd);
    return 0;
}

void wex_gf256_init(void) {
    if (gf_initialized) {
        return;
    }

    /* Generator g = 3, irreducible polynomial = 0x11B (x^8 + x^4 + x^3 + x + 1) */
    uint8_t x = 1;
    for (int i = 0; i < 255; i++) {
        exp_tbl[i] = x;
        exp_tbl[i + 255] = x;
        log_tbl[x] = (uint8_t)i;

        /* x = (x * 3) in GF(2^8) */
        uint8_t x2 = (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
        x = (uint8_t)(x2 ^ x);
    }

    log_tbl[0] = 0; /* Mathematically undefined, but mapped to 0 */
    gf_initialized = 1;
}

static inline uint8_t gf_add(uint8_t a, uint8_t b) {
    return (uint8_t)(a ^ b);
}

static inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    return exp_tbl[log_tbl[a] + log_tbl[b]];
}

static inline uint8_t gf_div(uint8_t a, uint8_t b) {
    if (a == 0) {
        return 0;
    }
    if (b == 0) {
        return 0; /* Avoid division by zero */
    }
    return exp_tbl[log_tbl[a] + 255 - log_tbl[b]];
}

static void compute_share_checksum(const wex_share_t *share, uint8_t out[WEX_CHECKSUM_LEN]) {
    /* Hash header fields (index, threshold, total) + data payload */
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, WEX_CHECKSUM_LEN);

    uint8_t hdr[3];
    hdr[0] = share->index;
    hdr[1] = share->threshold;
    hdr[2] = share->total;

    crypto_blake2b_update(&ctx, hdr, sizeof(hdr));
    crypto_blake2b_update(&ctx, share->data, WEX_KEY_LEN);
    crypto_blake2b_final(&ctx, out);
}

int wex_shamir_split(const uint8_t secret[WEX_KEY_LEN],
                     uint8_t k,
                     uint8_t n,
                     wex_share_t *shares_out) {
    if (!secret || !shares_out) {
        return -1;
    }
    if (k < 2 || n < k) {
        return -1;
    }

    wex_gf256_init();

    /* Initialize shares metadata */
    for (uint8_t i = 0; i < n; i++) {
        shares_out[i].index = (uint8_t)(i + 1); /* x in 1..n */
        shares_out[i].threshold = k;
        shares_out[i].total = n;
        memset(shares_out[i].data, 0, WEX_KEY_LEN);
    }

    /* Temporary buffer for random polynomial coefficients */
    uint8_t coeffs[256]; /* at most k-1 <= 254 bytes */

    for (size_t byte_idx = 0; byte_idx < WEX_KEY_LEN; byte_idx++) {
        /* Generate k-1 random coefficients: c_1, c_2, ..., c_{k-1} */
        if (wex_randombytes(coeffs, (size_t)(k - 1)) != 0) {
            wex_clean(coeffs, sizeof(coeffs));
            return -1;
        }

        /* Evaluate polynomial P(x) = secret[byte_idx] + sum_{j=1}^{k-1}(c_j * x^j) */
        for (uint8_t share_idx = 0; share_idx < n; share_idx++) {
            uint8_t x_val = shares_out[share_idx].index;
            uint8_t y_val = secret[byte_idx];
            uint8_t x_pow = x_val;

            for (uint8_t deg = 0; deg < (k - 1); deg++) {
                y_val = gf_add(y_val, gf_mul(coeffs[deg], x_pow));
                x_pow = gf_mul(x_pow, x_val);
            }

            shares_out[share_idx].data[byte_idx] = y_val;
        }
    }

    wex_clean(coeffs, sizeof(coeffs));

    /* Compute BLAKE2b authentication checksums for all shares */
    for (uint8_t i = 0; i < n; i++) {
        compute_share_checksum(&shares_out[i], shares_out[i].checksum);
    }

    return 0;
}

bool wex_shamir_verify_share(const wex_share_t *share) {
    if (!share) {
        return false;
    }
    if (share->index == 0 || share->threshold < 2 || share->total < share->threshold) {
        return false;
    }

    uint8_t expected[WEX_CHECKSUM_LEN];
    compute_share_checksum(share, expected);

    int match = crypto_verify16(share->checksum, expected);
    return (match == 0);
}

int wex_shamir_combine(const wex_share_t *shares[],
                       uint8_t num_shares,
                       uint8_t recovered_secret[WEX_KEY_LEN]) {
    if (!shares || !recovered_secret) {
        return -1;
    }
    if (num_shares < 2) {
        return -1;
    }

    wex_gf256_init();

    /* Verify all incoming shares against tampering */
    for (uint8_t i = 0; i < num_shares; i++) {
        if (!wex_shamir_verify_share(shares[i])) {
            return -2; /* Poisoned or corrupt share */
        }
    }

    uint8_t k = shares[0]->threshold;
    if (num_shares < k) {
        return -1; /* Insufficient shares to meet threshold */
    }

    /* Check for duplicate share indices */
    for (uint8_t i = 0; i < k; i++) {
        for (uint8_t j = i + 1; j < k; j++) {
            if (shares[i]->index == shares[j]->index) {
                return -3; /* Duplicate index detected */
            }
        }
    }

    /* Reconstruct secret using Lagrange interpolation at x = 0 */
    for (size_t byte_idx = 0; byte_idx < WEX_KEY_LEN; byte_idx++) {
        uint8_t secret_byte = 0;

        for (uint8_t j = 0; j < k; j++) {
            uint8_t y_j = shares[j]->data[byte_idx];
            uint8_t x_j = shares[j]->index;

            uint8_t num = 1;
            uint8_t den = 1;

            for (uint8_t m = 0; m < k; m++) {
                if (m == j) {
                    continue;
                }
                uint8_t x_m = shares[m]->index;

                /* In GF(2^8): (0 - x_m) is x_m, and (x_j - x_m) is (x_j ^ x_m) */
                num = gf_mul(num, x_m);
                den = gf_mul(den, gf_add(x_j, x_m));
            }

            uint8_t basis = gf_div(num, den);
            secret_byte = gf_add(secret_byte, gf_mul(y_j, basis));
        }

        recovered_secret[byte_idx] = secret_byte;
    }

    return 0;
}

int wex_encrypt(const uint8_t key[WEX_KEY_LEN],
                const uint8_t *plaintext,
                size_t len,
                uint8_t *ciphertext,
                uint8_t nonce[WEX_NONCE_LEN],
                uint8_t mac[WEX_MAC_LEN]) {
    if (!key || !ciphertext || (len > 0 && !plaintext) || !nonce || !mac) {
        return -1;
    }

    if (wex_randombytes(nonce, WEX_NONCE_LEN) != 0) {
        return -1;
    }

    crypto_aead_lock(ciphertext, mac, key, nonce, NULL, 0, plaintext, len);
    return 0;
}

int wex_decrypt(const uint8_t key[WEX_KEY_LEN],
                const uint8_t *ciphertext,
                size_t len,
                const uint8_t nonce[WEX_NONCE_LEN],
                const uint8_t mac[WEX_MAC_LEN],
                uint8_t *plaintext) {
    if (!key || (len > 0 && !ciphertext) || !nonce || !mac || !plaintext) {
        return -1;
    }

    return crypto_aead_unlock(plaintext, mac, key, nonce, NULL, 0, ciphertext, len);
}
