#ifndef WEX_PERSIST_H
#define WEX_PERSIST_H

#include "wex_models.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define WEX_ID_FILE_MAGIC 0x57455849 /* "WEXI" */
#define WEX_JOURNAL_MAGIC 0x5745584A /* "WEXJ" */
#define WEX_REC_MAGIC     0x52454331 /* "REC1" */

/* Identity file persistence */
int wex_identity_save(const wex_identity_t *ident, const char *path);
int wex_identity_load(wex_identity_t *ident, const char *path);
int wex_identity_load_or_create(wex_identity_t *ident, const char *path, const char *default_nick);

/* High-performance append-only message journal */
typedef struct wex_journal wex_journal_t;

int wex_journal_open(wex_journal_t **out_journal, const char *path);
int wex_journal_append(wex_journal_t *journal, const wex_chat_msg_t *msg);
size_t wex_journal_read_recent(wex_journal_t *journal, const uint8_t space_id[32], const char *channel, wex_chat_msg_t out_msgs[], size_t max_msgs);
size_t wex_journal_count(wex_journal_t *journal);
void wex_journal_close(wex_journal_t *journal);

/* Ensure a directory hierarchy exists (mkdir -p helper) */
int wex_mkdir_p(const char *dir_path);

#endif /* WEX_PERSIST_H */
