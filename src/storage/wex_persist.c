#define _GNU_SOURCE
#include "wex_persist.h"
#include "wex_crypto.h"
#include "../../vendor/monocypher/monocypher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t  seed[32];
    uint8_t  sign_pk[32];
    uint8_t  sign_sk[64];
    uint8_t  x25519_pk[32];
    uint8_t  x25519_sk[32];
    char     nickname[WEX_NAME_LEN];
    char     status[32];
    uint8_t  checksum[16];
} wex_id_file_disk_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
} wex_journal_hdr_disk_t;

typedef struct {
    uint32_t magic;
    uint32_t total_len;
    uint64_t timestamp_ms;
    uint8_t  space_id[32];
    char     channel[WEX_NAME_LEN];
    char     author[WEX_NAME_LEN];
    uint8_t  author_pk[32];
    uint8_t  enc_flags;
    uint8_t  nonce[24];
    uint8_t  mac[16];
    uint8_t  signature[64];
    uint16_t content_len;
} wex_journal_rec_hdr_t;
#pragma pack(pop)

struct wex_journal {
    int fd;
    char path[512];
    size_t count;
};

int wex_mkdir_p(const char *dir_path) {
    if (!dir_path || strlen(dir_path) == 0) return -1;

    char tmp[512];
    strncpy(tmp, dir_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    size_t len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int wex_identity_save(const wex_identity_t *ident, const char *path) {
    if (!ident || !path) return -1;

    /* Extract directory and ensure it exists */
    char dir_copy[512];
    strncpy(dir_copy, path, sizeof(dir_copy) - 1);
    dir_copy[sizeof(dir_copy) - 1] = '\0';
    char *last_slash = strrchr(dir_copy, '/');
    if (last_slash) {
        *last_slash = '\0';
        wex_mkdir_p(dir_copy);
    }

    wex_id_file_disk_t disk;
    memset(&disk, 0, sizeof(disk));
    disk.magic = htonl(WEX_ID_FILE_MAGIC);
    disk.version = htons(1);
    memcpy(disk.seed, ident->seed, 32);
    memcpy(disk.sign_pk, ident->sign_pk, 32);
    memcpy(disk.sign_sk, ident->sign_sk, 64);
    memcpy(disk.x25519_pk, ident->x25519_pk, 32);
    memcpy(disk.x25519_sk, ident->x25519_sk, 32);
    strncpy(disk.nickname, ident->nickname, sizeof(disk.nickname) - 1);
    strncpy(disk.status, ident->status, sizeof(disk.status) - 1);

    /* Compute BLAKE2b verification tag */
    size_t payload_len = sizeof(disk) - sizeof(disk.checksum);
    crypto_blake2b(disk.checksum, sizeof(disk.checksum), (const uint8_t *)&disk, payload_len);

    /* Atomic write via temp file */
    char tmp_path[560];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        wex_clean(&disk, sizeof(disk));
        return -1;
    }

    ssize_t written = write(fd, &disk, sizeof(disk));
    wex_clean(&disk, sizeof(disk));

    if (written != (ssize_t)sizeof(disk)) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    fsync(fd);
    close(fd);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

int wex_identity_load(wex_identity_t *ident, const char *path) {
    if (!ident || !path) return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    wex_id_file_disk_t disk;
    ssize_t r = read(fd, &disk, sizeof(disk));
    close(fd);

    if (r != (ssize_t)sizeof(disk)) {
        return -1;
    }

    if (ntohl(disk.magic) != WEX_ID_FILE_MAGIC || ntohs(disk.version) != 1) {
        return -1;
    }

    uint8_t expected_tag[16];
    size_t payload_len = sizeof(disk) - sizeof(disk.checksum);
    crypto_blake2b(expected_tag, sizeof(expected_tag), (const uint8_t *)&disk, payload_len);

    if (crypto_verify16(expected_tag, disk.checksum) != 0) {
        return -2; /* Integrity failure / Corrupted key file */
    }

    memcpy(ident->seed, disk.seed, 32);
    memcpy(ident->sign_pk, disk.sign_pk, 32);
    memcpy(ident->sign_sk, disk.sign_sk, 64);
    memcpy(ident->x25519_pk, disk.x25519_pk, 32);
    memcpy(ident->x25519_sk, disk.x25519_sk, 32);
    strncpy(ident->nickname, disk.nickname, sizeof(ident->nickname) - 1);
    ident->nickname[sizeof(ident->nickname) - 1] = '\0';
    strncpy(ident->status, disk.status, sizeof(ident->status) - 1);
    ident->status[sizeof(ident->status) - 1] = '\0';

    wex_clean(&disk, sizeof(disk));
    return 0;
}

int wex_identity_load_or_create(wex_identity_t *ident, const char *path, const char *default_nick) {
    if (!ident || !path) return -1;

    if (access(path, F_OK) == 0) {
        int res = wex_identity_load(ident, path);
        if (res == 0) return 0;
        /* If corrupted, return error to avoid silently overwriting existing private keys */
        return res;
    }

    /* Initialize fresh identity */
    if (wex_identity_init(ident, NULL, default_nick) != 0) {
        return -1;
    }

    return wex_identity_save(ident, path);
}

int wex_journal_open(wex_journal_t **out_journal, const char *path) {
    if (!out_journal || !path) return -1;

    /* Ensure directory exists */
    char dir_copy[512];
    strncpy(dir_copy, path, sizeof(dir_copy) - 1);
    dir_copy[sizeof(dir_copy) - 1] = '\0';
    char *last_slash = strrchr(dir_copy, '/');
    if (last_slash) {
        *last_slash = '\0';
        wex_mkdir_p(dir_copy);
    }

    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }

    wex_journal_t *j = calloc(1, sizeof(wex_journal_t));
    if (!j) {
        close(fd);
        return -1;
    }
    j->fd = fd;
    strncpy(j->path, path, sizeof(j->path) - 1);

    if (st.st_size == 0) {
        /* Write journal header */
        wex_journal_hdr_disk_t hdr;
        hdr.magic = htonl(WEX_JOURNAL_MAGIC);
        hdr.version = htons(1);
        if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
            close(fd);
            free(j);
            return -1;
        }
        j->count = 0;
    } else {
        /* Read and verify journal header */
        wex_journal_hdr_disk_t hdr;
        if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
            close(fd);
            free(j);
            return -1;
        }
        if (ntohl(hdr.magic) != WEX_JOURNAL_MAGIC || ntohs(hdr.version) != 1) {
            close(fd);
            free(j);
            return -1;
        }

        /* Scan count of valid records */
        size_t count = 0;
        while (1) {
            wex_journal_rec_hdr_t rh;
            ssize_t n = read(fd, &rh, sizeof(rh));
            if (n <= 0) break;
            if (n < (ssize_t)sizeof(rh) || ntohl(rh.magic) != WEX_REC_MAGIC) break;

            uint32_t tlen = ntohl(rh.total_len);
            if (tlen < sizeof(rh) + 16) break;

            off_t cur = lseek(fd, tlen - sizeof(rh), SEEK_CUR);
            if (cur < 0) break;
            count++;
        }
        j->count = count;
    }

    *out_journal = j;
    return 0;
}

int wex_journal_append(wex_journal_t *journal, const wex_chat_msg_t *msg) {
    if (!journal || journal->fd < 0 || !msg) return -1;

    size_t cont_len = strlen(msg->content);
    if (cont_len > WEX_MAX_MSG_LEN) cont_len = WEX_MAX_MSG_LEN;

    uint32_t rec_len = (uint32_t)(sizeof(wex_journal_rec_hdr_t) + cont_len + 16);
    uint8_t *rec_buf = malloc(rec_len);
    if (!rec_buf) return -1;

    wex_journal_rec_hdr_t *rh = (wex_journal_rec_hdr_t *)rec_buf;
    rh->magic = htonl(WEX_REC_MAGIC);
    rh->total_len = htonl(rec_len);
    rh->timestamp_ms = msg->timestamp_ms;
    memcpy(rh->space_id, msg->space_id, 32);
    strncpy(rh->channel, msg->channel, sizeof(rh->channel) - 1);
    strncpy(rh->author, msg->author, sizeof(rh->author) - 1);
    memcpy(rh->author_pk, msg->author_pk, 32);
    rh->enc_flags = msg->enc_flags;
    memcpy(rh->nonce, msg->nonce, 24);
    memcpy(rh->mac, msg->mac, 16);
    memcpy(rh->signature, msg->signature, 64);
    rh->content_len = htons((uint16_t)cont_len);

    memcpy(rec_buf + sizeof(wex_journal_rec_hdr_t), msg->content, cont_len);

    /* Tag over record */
    uint8_t tag[16];
    crypto_blake2b(tag, 16, rec_buf, rec_len - 16);
    memcpy(rec_buf + rec_len - 16, tag, 16);

    /* Append to end */
    lseek(journal->fd, 0, SEEK_END);
    ssize_t w = write(journal->fd, rec_buf, rec_len);
    free(rec_buf);

    if (w != (ssize_t)rec_len) {
        return -1;
    }

    journal->count++;
    return 0;
}

size_t wex_journal_read_recent(wex_journal_t *journal, const uint8_t space_id[32], const char *channel,
                               wex_chat_msg_t out_msgs[], size_t max_msgs) {
    if (!journal || journal->fd < 0 || !out_msgs || max_msgs == 0) return 0;

    lseek(journal->fd, sizeof(wex_journal_hdr_disk_t), SEEK_SET);

    /* Read all matching messages */
    size_t match_capacity = 256;
    wex_chat_msg_t *all_matches = malloc(match_capacity * sizeof(wex_chat_msg_t));
    if (!all_matches) return 0;
    size_t match_count = 0;

    while (1) {
        wex_journal_rec_hdr_t rh;
        ssize_t n = read(journal->fd, &rh, sizeof(rh));
        if (n < (ssize_t)sizeof(rh)) break;
        if (ntohl(rh.magic) != WEX_REC_MAGIC) break;

        uint32_t tlen = ntohl(rh.total_len);
        if (tlen < sizeof(rh) + 16) break;

        uint16_t clen = ntohs(rh.content_len);
        if ((size_t)tlen != sizeof(rh) + clen + 16) {
            lseek(journal->fd, tlen - sizeof(rh), SEEK_CUR);
            continue;
        }

        uint8_t *rest = malloc(clen + 16);
        if (!rest) break;
        if (read(journal->fd, rest, clen + 16) != (ssize_t)(clen + 16)) {
            free(rest);
            break;
        }

        bool match = true;
        if (space_id && memcmp(rh.space_id, space_id, 32) != 0) {
            match = false;
        }
        if (channel && strncmp(rh.channel, channel, WEX_NAME_LEN) != 0) {
            match = false;
        }

        if (match) {
            if (match_count >= match_capacity) {
                match_capacity *= 2;
                wex_chat_msg_t *grown = realloc(all_matches, match_capacity * sizeof(wex_chat_msg_t));
                if (!grown) {
                    free(rest);
                    break;
                }
                all_matches = grown;
            }

            wex_chat_msg_t *m = &all_matches[match_count++];
            memset(m, 0, sizeof(*m));
            memcpy(m->space_id, rh.space_id, 32);
            strncpy(m->channel, rh.channel, sizeof(m->channel) - 1);
            strncpy(m->author, rh.author, sizeof(m->author) - 1);
            memcpy(m->author_pk, rh.author_pk, 32);
            m->timestamp_ms = rh.timestamp_ms;
            m->enc_flags = rh.enc_flags;
            memcpy(m->nonce, rh.nonce, 24);
            memcpy(m->mac, rh.mac, 16);
            memcpy(m->signature, rh.signature, 64);

            size_t copy_c = (clen < WEX_MAX_MSG_LEN - 1) ? clen : (WEX_MAX_MSG_LEN - 1);
            memcpy(m->content, rest, copy_c);
            m->content[copy_c] = '\0';
        }

        free(rest);
    }

    /* Select recent up to max_msgs */
    size_t to_copy = (match_count < max_msgs) ? match_count : max_msgs;
    size_t start_idx = (match_count > max_msgs) ? (match_count - max_msgs) : 0;

    for (size_t i = 0; i < to_copy; i++) {
        out_msgs[i] = all_matches[start_idx + i];
    }

    free(all_matches);
    return to_copy;
}

size_t wex_journal_count(wex_journal_t *journal) {
    return journal ? journal->count : 0;
}

void wex_journal_close(wex_journal_t *journal) {
    if (!journal) return;
    if (journal->fd >= 0) {
        close(journal->fd);
        journal->fd = -1;
    }
    free(journal);
}
