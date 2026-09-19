#include "store.h"

#include "log.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define HISTORY_MAX_LINES 5000

static char history_path[512];
static char contacts_path[512];

static oma_contact *contacts;
static size_t contacts_len;
static size_t contacts_cap;

static int contact_cmp(const void *a, const void *b)
{
    const oma_contact *ca = a, *cb = b;
    int by_name = strcasecmp(ca->name, cb->name);

    /* Two contacts can share a display name; the number breaks the tie so the
     * order stays stable between reads. */
    return by_name != 0 ? by_name : strcmp(ca->number, cb->number);
}

static int contacts_reserve(size_t want)
{
    oma_contact *grown;
    size_t next;

    if (want <= contacts_cap)
        return 0;
    next = contacts_cap == 0 ? 16 : contacts_cap * 2;
    while (next < want)
        next *= 2;
    grown = realloc(contacts, next * sizeof(*contacts));
    if (grown == NULL)
        return -1;
    contacts = grown;
    contacts_cap = next;
    return 0;
}

static int contacts_save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *array;
    char *text;
    size_t i;
    int rc;

    if (root == NULL)
        return -1;
    array = cJSON_AddArrayToObject(root, "contacts");
    for (i = 0; i < contacts_len; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", contacts[i].id);
        cJSON_AddStringToObject(item, "name", contacts[i].name);
        cJSON_AddStringToObject(item, "number", contacts[i].number);
        cJSON_AddNumberToObject(item, "created_at", (double)contacts[i].created_at);
        cJSON_AddItemToArray(array, item);
    }
    text = cJSON_Print(root);
    cJSON_Delete(root);
    if (text == NULL)
        return -1;

    rc = oma_write_atomic(contacts_path, text, strlen(text), 0600);
    free(text);
    return rc;
}

static void contacts_load(void)
{
    char *text = oma_read_file(contacts_path, NULL);
    cJSON *root, *array, *item;

    if (text == NULL)
        return;

    root = cJSON_Parse(text);
    free(text);
    if (root == NULL) {
        oma_warn("store: %s is not valid JSON, starting with no contacts",
                 contacts_path);
        return;
    }

    array = cJSON_GetObjectItemCaseSensitive(root, "contacts");
    cJSON_ArrayForEach(item, array) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *number = cJSON_GetObjectItemCaseSensitive(item, "number");
        const cJSON *created = cJSON_GetObjectItemCaseSensitive(item, "created_at");
        oma_contact *slot;

        if (!cJSON_IsString(id) || !cJSON_IsString(name) || !cJSON_IsString(number))
            continue;
        if (contacts_reserve(contacts_len + 1) != 0)
            break;

        slot = &contacts[contacts_len++];
        memset(slot, 0, sizeof(*slot));
        oma_strlcpy(slot->id, id->valuestring, sizeof(slot->id));
        oma_strlcpy(slot->name, name->valuestring, sizeof(slot->name));
        oma_strlcpy(slot->number, number->valuestring, sizeof(slot->number));
        slot->created_at = cJSON_IsNumber(created) ? (int64_t)created->valuedouble
                                                   : oma_now();
    }
    cJSON_Delete(root);

    if (contacts_len > 1)
        qsort(contacts, contacts_len, sizeof(*contacts), contact_cmp);
    oma_info("store: %zu contacts loaded", contacts_len);
}

int oma_store_init(void)
{
    if (oma_path_data(history_path, sizeof(history_path), "history.jsonl") != 0)
        return -1;
    if (oma_path_data(contacts_path, sizeof(contacts_path), "contacts.json") != 0)
        return -1;
    contacts_load();
    return 0;
}

void oma_store_shutdown(void)
{
    free(contacts);
    contacts = NULL;
    contacts_len = 0;
    contacts_cap = 0;
}

int oma_store_history_append(const oma_history_item *item)
{
    cJSON *obj = cJSON_CreateObject();
    char *line;
    FILE *fp;

    if (obj == NULL)
        return -1;
    cJSON_AddNumberToObject(obj, "ts", (double)item->ts);
    cJSON_AddStringToObject(obj, "direction", item->direction);
    cJSON_AddStringToObject(obj, "number", item->number);
    cJSON_AddStringToObject(obj, "display", item->display);
    cJSON_AddStringToObject(obj, "reason", item->reason);
    cJSON_AddNumberToObject(obj, "duration_sec", item->duration_sec);
    cJSON_AddNumberToObject(obj, "status_code", item->status_code);

    line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (line == NULL)
        return -1;

    fp = fopen(history_path, "ae");
    if (fp == NULL) {
        oma_err("store: cannot append to %s: %m", history_path);
        free(line);
        return -1;
    }
    fprintf(fp, "%s\n", line);
    free(line);
    if (fclose(fp) != 0) {
        oma_err("store: cannot flush %s: %m", history_path);
        return -1;
    }
    return 0;
}

/* Parse the file into an array oldest-first. Returns NULL when there is none. */
static cJSON *history_read_all(void)
{
    char *text = oma_read_file(history_path, NULL);
    cJSON *array;
    char *line, *saveptr = NULL;

    if (text == NULL)
        return NULL;

    array = cJSON_CreateArray();
    if (array == NULL) {
        free(text);
        return NULL;
    }

    for (line = strtok_r(text, "\n", &saveptr); line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {
        cJSON *item;

        if (line[0] == '\0')
            continue;
        item = cJSON_Parse(line);
        /* One corrupt line must not cost the user the rest of the history. */
        if (item == NULL) {
            oma_warn("store: skipping a malformed history line");
            continue;
        }
        cJSON_AddItemToArray(array, item);
    }

    free(text);
    return array;
}

cJSON *oma_store_history_list(int limit, int offset, int *total)
{
    cJSON *all = history_read_all();
    cJSON *page = cJSON_CreateArray();
    int count, i;

    if (page == NULL) {
        cJSON_Delete(all);
        return NULL;
    }
    count = all != NULL ? cJSON_GetArraySize(all) : 0;
    if (total != NULL)
        *total = count;
    if (all == NULL)
        return page;

    if (limit <= 0)
        limit = 50;
    if (offset < 0)
        offset = 0;

    /* The file grows oldest-first; clients want newest-first. */
    for (i = count - 1 - offset; i >= 0 && cJSON_GetArraySize(page) < limit; i--) {
        cJSON *item = cJSON_GetArrayItem(all, i);
        if (item != NULL)
            cJSON_AddItemToArray(page, cJSON_Duplicate(item, 1));
    }

    cJSON_Delete(all);
    return page;
}

int oma_store_history_clear(void)
{
    FILE *fp = fopen(history_path, "we");

    if (fp == NULL) {
        oma_err("store: cannot truncate %s: %m", history_path);
        return -1;
    }
    return fclose(fp) == 0 ? 0 : -1;
}

void oma_store_history_trim(void)
{
    cJSON *all = history_read_all();
    int count = all != NULL ? cJSON_GetArraySize(all) : 0;
    FILE *fp;
    int i;

    if (count <= HISTORY_MAX_LINES) {
        cJSON_Delete(all);
        return;
    }

    fp = fopen(history_path, "we");
    if (fp == NULL) {
        cJSON_Delete(all);
        return;
    }
    for (i = count - HISTORY_MAX_LINES; i < count; i++) {
        char *line = cJSON_PrintUnformatted(cJSON_GetArrayItem(all, i));
        if (line != NULL) {
            fprintf(fp, "%s\n", line);
            free(line);
        }
    }
    fclose(fp);
    cJSON_Delete(all);
    oma_info("store: history trimmed to the newest %d calls", HISTORY_MAX_LINES);
}

cJSON *oma_store_contacts_list(void)
{
    cJSON *array = cJSON_CreateArray();
    size_t i;

    if (array == NULL)
        return NULL;
    for (i = 0; i < contacts_len; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", contacts[i].id);
        cJSON_AddStringToObject(item, "name", contacts[i].name);
        cJSON_AddStringToObject(item, "number", contacts[i].number);
        cJSON_AddNumberToObject(item, "created_at", (double)contacts[i].created_at);
        cJSON_AddItemToArray(array, item);
    }
    return array;
}

static oma_contact *contact_find(const char *id)
{
    size_t i;

    for (i = 0; i < contacts_len; i++) {
        if (strcmp(contacts[i].id, id) == 0)
            return &contacts[i];
    }
    return NULL;
}

int oma_store_contact_upsert(const char *id, const char *name, const char *number,
                             oma_contact *out, char *err, size_t errlen)
{
    oma_contact *slot;

    if (name == NULL || name[0] == '\0') {
        snprintf(err, errlen, "name is required");
        return -1;
    }
    if (number == NULL || number[0] == '\0') {
        snprintf(err, errlen, "number is required");
        return -1;
    }

    if (id != NULL && id[0] != '\0') {
        slot = contact_find(id);
        if (slot == NULL) {
            snprintf(err, errlen, "no contact with id %s", id);
            return -1;
        }
    } else {
        if (contacts_reserve(contacts_len + 1) != 0) {
            snprintf(err, errlen, "out of memory");
            return -1;
        }
        slot = &contacts[contacts_len++];
        memset(slot, 0, sizeof(*slot));
        oma_id_new(slot->id, sizeof(slot->id), "c_");
        slot->created_at = oma_now();
    }

    oma_strlcpy(slot->name, name, sizeof(slot->name));
    oma_strlcpy(slot->number, number, sizeof(slot->number));

    if (out != NULL)
        *out = *slot;

    if (contacts_len > 1)
        qsort(contacts, contacts_len, sizeof(*contacts), contact_cmp);
    if (contacts_save() != 0) {
        snprintf(err, errlen, "cannot write %s", contacts_path);
        return -1;
    }
    return 0;
}

int oma_store_contact_delete(const char *id)
{
    oma_contact *slot;
    size_t index;

    if (id == NULL || id[0] == '\0')
        return -1;
    slot = contact_find(id);
    if (slot == NULL)
        return -1;

    index = (size_t)(slot - contacts);
    memmove(&contacts[index], &contacts[index + 1],
            (contacts_len - index - 1) * sizeof(*contacts));
    contacts_len--;
    return contacts_save();
}

const char *oma_store_contact_name_for_number(const char *number)
{
    size_t i;

    if (number == NULL || number[0] == '\0')
        return NULL;
    for (i = 0; i < contacts_len; i++) {
        if (strcmp(contacts[i].number, number) == 0)
            return contacts[i].name;
    }
    return NULL;
}
