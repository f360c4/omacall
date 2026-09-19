#ifndef OMA_STORE_H
#define OMA_STORE_H

#include <cjson/cJSON.h>
#include <stdint.h>
#include <stddef.h>

/* Call history (append-only JSONL) and contacts (one JSON object, rewritten
 * atomically). History is read from disk on demand — it is only touched when a
 * call ends or a client asks for it. Contacts are small and looked up on every
 * incoming call, so they stay in memory. */

typedef struct {
    int64_t ts;
    char    direction[4];    /* "in" or "out" */
    char    number[128];
    char    display[128];
    char    reason[16];      /* completed, missed, rejected, busy, failed, cancelled */
    int     duration_sec;
    int     status_code;
} oma_history_item;

typedef struct {
    char    id[32];
    char    name[128];
    char    number[128];
    int64_t created_at;
} oma_contact;

int  oma_store_init(void);
void oma_store_shutdown(void);

int  oma_store_history_append(const oma_history_item *item);
/* Newest first. Caller owns the returned array; *total gets the full count. */
cJSON *oma_store_history_list(int limit, int offset, int *total);
int  oma_store_history_clear(void);
/* Rewrite the file with only the newest entries once it grows past the cap.
 * Called after appending, so the file never grows without bound. */
void oma_store_history_trim(void);

/* Sorted by name, case-insensitive. Caller owns the array. */
cJSON *oma_store_contacts_list(void);
/* `id` empty or NULL creates; otherwise updates that contact. */
int  oma_store_contact_upsert(const char *id, const char *name, const char *number,
                              oma_contact *out, char *err, size_t errlen);
int  oma_store_contact_delete(const char *id);
/* Name of the contact whose number matches, or NULL. */
const char *oma_store_contact_name_for_number(const char *number);

#endif /* OMA_STORE_H */
