#include "ipc.h"

#include "calls.h"
#include "log.h"
#include "sip.h"
#include "store.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_CLIENTS   8
#define MAX_LINE      (64 * 1024)
#define HELLO_HISTORY 20

/* Bounds on replies waiting to be written. A client that sends requests and
 * never reads the answers would otherwise make the daemon buffer them without
 * limit — a same-user process on the 0600 socket, or simply a script that
 * forgot to read. Above SOFT the daemon stops reading from that client, so it
 * stops producing for it; above HARD it gives up on the client. TOTAL caps
 * every client together. */
#define QUEUE_SOFT_LIMIT  (256 * 1024)
#define QUEUE_HARD_LIMIT  (1024 * 1024)
#define QUEUE_TOTAL_LIMIT (2 * 1024 * 1024)

typedef struct {
    int    fd;
    char   in[MAX_LINE];
    size_t in_len;
    char  *out;
    size_t out_len;
    size_t out_cap;
    bool   overflowed;   /* line too long: drop until the next newline */
} client;

static int listen_fd = -1;
static char socket_path[512];
static client clients[MAX_CLIENTS];
static size_t client_count;
static size_t queued_total;   /* bytes waiting across every client */

static oma_config *live_config;
static char live_config_path[512];
static bool quit_requested;

void oma_ipc_bind_config(oma_config *cfg, const char *config_path)
{
    live_config = cfg;
    oma_strlcpy(live_config_path, config_path, sizeof(live_config_path));
}

bool oma_ipc_quit_requested(void)
{
    return quit_requested;
}

/* ------------------------------------------------------------------ output */

static void client_close(client *c)
{
    if (c->fd >= 0)
        close(c->fd);
    queued_total -= c->out_len;
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static void client_queue(client *c, const char *line, size_t len)
{
    if (c->fd < 0)
        return;

    if (c->out_len + len + 1 > QUEUE_HARD_LIMIT) {
        oma_warn("ipc: client is not reading its replies (%zu bytes queued), "
                 "dropping it", c->out_len);
        client_close(c);
        return;
    }
    if (queued_total + len + 1 > QUEUE_TOTAL_LIMIT) {
        oma_warn("ipc: %zu bytes queued across all clients, dropping the one "
                 "that pushed it over", queued_total);
        client_close(c);
        return;
    }

    if (c->out_len + len + 1 > c->out_cap) {
        size_t next = c->out_cap == 0 ? 8192 : c->out_cap;
        char *grown;

        while (next < c->out_len + len + 1)
            next *= 2;
        grown = realloc(c->out, next);
        if (grown == NULL) {
            oma_err("ipc: out of memory queueing a reply, dropping the client");
            client_close(c);
            return;
        }
        c->out = grown;
        c->out_cap = next;
    }
    memcpy(c->out + c->out_len, line, len);
    c->out_len += len;
    c->out[c->out_len++] = '\n';
    queued_total += len + 1;
}

/* Push as much of the queue as the socket accepts right now. */
static void client_flush(client *c)
{
    while (c->out_len > 0) {
        /* MSG_NOSIGNAL as well as the ignored SIGPIPE in main: this way the
         * write is safe even if some future caller forgets the handler. */
        ssize_t sent = send(c->fd, c->out, c->out_len, MSG_NOSIGNAL);

        if (sent < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            oma_dbg("ipc: write failed, closing client: %m");
            client_close(c);
            return;
        }
        memmove(c->out, c->out + sent, c->out_len - (size_t)sent);
        c->out_len -= (size_t)sent;
        queued_total -= (size_t)sent;
    }
}

/* Serialize `obj` (taking ownership) and queue it on `c`. */
static void client_send(client *c, cJSON *obj)
{
    char *line = cJSON_PrintUnformatted(obj);

    cJSON_Delete(obj);
    if (line == NULL)
        return;
    client_queue(c, line, strlen(line));
    free(line);
    client_flush(c);
}

static void reply_ok(client *c, int id, cJSON *result)
{
    cJSON *msg = cJSON_CreateObject();

    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddBoolToObject(msg, "ok", true);
    cJSON_AddItemToObject(msg, "result", result != NULL ? result : cJSON_CreateObject());
    client_send(c, msg);
}

static void reply_err(client *c, int id, const char *code, const char *message)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON *err = cJSON_CreateObject();

    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddBoolToObject(msg, "ok", false);
    cJSON_AddStringToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(msg, "error", err);
    client_send(c, msg);
}

void oma_ipc_broadcast(const char *event, cJSON *data)
{
    cJSON *msg = cJSON_CreateObject();
    char *line;
    size_t i;

    cJSON_AddStringToObject(msg, "event", event);
    cJSON_AddItemToObject(msg, "data", data != NULL ? data : cJSON_CreateObject());
    line = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (line == NULL)
        return;

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0)
            continue;
        client_queue(&clients[i], line, strlen(line));
        client_flush(&clients[i]);
    }
    free(line);
}

/* ---------------------------------------------------------------- commands */

static const char *arg_string(const cJSON *args, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, key);

    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int arg_int(const cJSON *args, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, key);

    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static cJSON *status_object(void)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *account = cJSON_CreateObject();

    cJSON_AddStringToObject(obj, "state", oma_state_name(oma_sip_state()));
    cJSON_AddBoolToObject(obj, "registered", oma_sip_is_registered());
    oma_sip_account_json(account);
    cJSON_AddItemToObject(obj, "account", account);
    cJSON_AddItemToObject(obj, "call", oma_calls_json());
    return obj;
}

static void cmd_hello(client *c, int id, const cJSON *args)
{
    cJSON *result = status_object();
    const char *who = arg_string(args, "client");

    cJSON_AddStringToObject(result, "daemon_version", OMACALL_VERSION);
    cJSON_AddNumberToObject(result, "protocol", OMA_PROTOCOL_VERSION);
    cJSON_AddItemToObject(result, "history_tail",
                          oma_store_history_list(HELLO_HISTORY, 0, NULL));
    cJSON_AddItemToObject(result, "contacts", oma_store_contacts_list());
    oma_dbg("ipc: hello from %s", who != NULL ? who : "an unnamed client");
    reply_ok(c, id, result);
}

static void cmd_status(client *c, int id, const cJSON *args)
{
    (void)args;
    reply_ok(c, id, status_object());
}

static void cmd_config_get(client *c, int id, const cJSON *args)
{
    char *text;
    cJSON *result;

    (void)args;
    text = oma_config_to_json(live_config, true);
    if (text == NULL) {
        reply_err(c, id, "internal", "cannot serialize the configuration");
        return;
    }
    result = cJSON_Parse(text);
    free(text);
    reply_ok(c, id, result);
}

static void cmd_config_set(client *c, int id, const cJSON *args)
{
    char err[256] = "";
    char *text;

    if (!cJSON_IsObject(args)) {
        reply_err(c, id, "bad_args", "expected a config object");
        return;
    }

    text = cJSON_PrintUnformatted(args);
    if (text == NULL) {
        reply_err(c, id, "internal", "cannot read the request");
        return;
    }
    if (oma_config_merge_json(live_config, text, err, sizeof(err)) != 0) {
        free(text);
        reply_err(c, id, "bad_args", err);
        return;
    }
    free(text);

    if (oma_config_save(live_config, live_config_path) != 0) {
        reply_err(c, id, "io", "cannot write the configuration file");
        return;
    }
    if (oma_sip_reconfigure(live_config, err, sizeof(err)) != 0) {
        reply_err(c, id, "internal", err);
        return;
    }

    reply_ok(c, id, NULL);
    oma_ipc_broadcast("config.changed", NULL);
}

/* The saved accounts, without passwords: a client lists them to offer a
 * choice, and never needs to read one back. */
static void cmd_accounts_list(client *c, int id, const cJSON *args)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(result, "accounts");
    size_t i;

    (void)args;
    for (i = 0; i < live_config->account_count; i++) {
        const oma_profile *profile = &live_config->accounts[i];
        cJSON *entry = cJSON_CreateObject();

        cJSON_AddStringToObject(entry, "name", profile->name);
        cJSON_AddStringToObject(entry, "username", profile->account.username);
        cJSON_AddStringToObject(entry, "domain", profile->account.domain);
        cJSON_AddStringToObject(entry, "transport", profile->account.transport);
        cJSON_AddBoolToObject(entry, "password_set",
                              profile->account.password[0] != '\0');
        cJSON_AddBoolToObject(entry, "active",
                              strcmp(profile->name, live_config->active_account) == 0);
        cJSON_AddItemToArray(array, entry);
    }
    cJSON_AddStringToObject(result, "active", live_config->active_account);
    reply_ok(c, id, result);
}

/* Switch which account is registered. The old one is dropped first: two
 * registrations at once is exactly what this feature is not. */
static void cmd_accounts_select(client *c, int id, const cJSON *args)
{
    const char *name = arg_string(args, "name");
    char err[256] = "";

    if (name == NULL) {
        reply_err(c, id, "bad_args", "name is required");
        return;
    }
    if (oma_config_find_account(live_config, name) < 0) {
        reply_err(c, id, "bad_args", "no account with that name");
        return;
    }

    oma_strlcpy(live_config->active_account, name,
                sizeof(live_config->active_account));
    oma_config_apply_active(live_config);

    if (oma_config_save(live_config, live_config_path) != 0) {
        reply_err(c, id, "io", "cannot write the configuration file");
        return;
    }
    if (oma_sip_reconfigure(live_config, err, sizeof(err)) != 0) {
        reply_err(c, id, "internal", err);
        return;
    }

    oma_info("accounts: switched to \"%s\"", name);
    reply_ok(c, id, NULL);
    oma_ipc_broadcast("config.changed", NULL);
}

static void cmd_accounts_upsert(client *c, int id, const cJSON *args)
{
    const char *name = arg_string(args, "name");
    char err[256] = "";
    char *text;
    int index;

    if (name == NULL || name[0] == '\0') {
        reply_err(c, id, "bad_args", "name is required");
        return;
    }

    index = oma_config_find_account(live_config, name);
    if (index < 0) {
        if (live_config->account_count >= OMA_MAX_ACCOUNTS) {
            reply_err(c, id, "bad_args", "no room for another account");
            return;
        }
        index = (int)live_config->account_count++;
        memset(&live_config->accounts[index], 0, sizeof(live_config->accounts[index]));
        live_config->accounts[index].account.port = 5060;
        live_config->accounts[index].account.register_expires = 300;
        oma_strlcpy(live_config->accounts[index].account.transport, "udp",
                    sizeof(live_config->accounts[index].account.transport));
        live_config->accounts[index].tls.verify_server = true;
        oma_strlcpy(live_config->accounts[index].name, name,
                    sizeof(live_config->accounts[index].name));
    }

    /* Reuse the config parser rather than unpicking the fields here: the
     * request has the same shape as an account in the file. */
    text = cJSON_PrintUnformatted(args);
    if (text == NULL) {
        reply_err(c, id, "internal", "cannot read the request");
        return;
    }
    {
        cJSON *parsed = cJSON_Parse(text);

        free(text);
        if (parsed != NULL) {
            oma_config_take_account_json(parsed, &live_config->accounts[index].account,
                                         &live_config->accounts[index].tls);
            cJSON_Delete(parsed);
        }
    }

    oma_config_apply_active(live_config);
    if (oma_config_save(live_config, live_config_path) != 0) {
        reply_err(c, id, "io", "cannot write the configuration file");
        return;
    }
    if (oma_sip_reconfigure(live_config, err, sizeof(err)) != 0) {
        reply_err(c, id, "internal", err);
        return;
    }

    reply_ok(c, id, NULL);
    oma_ipc_broadcast("config.changed", NULL);
}

static void cmd_accounts_delete(client *c, int id, const cJSON *args)
{
    const char *name = arg_string(args, "name");
    char err[256] = "";
    int index;

    if (name == NULL) {
        reply_err(c, id, "bad_args", "name is required");
        return;
    }
    index = oma_config_find_account(live_config, name);
    if (index < 0) {
        reply_err(c, id, "bad_args", "no account with that name");
        return;
    }
    if (live_config->account_count == 1) {
        reply_err(c, id, "bad_args", "the last account cannot be deleted");
        return;
    }

    memmove(&live_config->accounts[index], &live_config->accounts[index + 1],
            (live_config->account_count - (size_t)index - 1)
            * sizeof(live_config->accounts[0]));
    live_config->account_count--;

    /* Deleting the active one falls back to the first, which apply_active
     * does; the registration follows. */
    oma_config_apply_active(live_config);
    if (oma_config_save(live_config, live_config_path) != 0) {
        reply_err(c, id, "io", "cannot write the configuration file");
        return;
    }
    if (oma_sip_reconfigure(live_config, err, sizeof(err)) != 0) {
        reply_err(c, id, "internal", err);
        return;
    }

    reply_ok(c, id, NULL);
    oma_ipc_broadcast("config.changed", NULL);
}

static void cmd_register(client *c, int id, const cJSON *args)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, "enabled");
    char err[256] = "";
    bool enabled = item == NULL || cJSON_IsTrue(item);

    if (!oma_config_has_account(live_config)) {
        reply_err(c, id, "no_account", "no SIP account is configured");
        return;
    }
    if (oma_sip_set_registration(enabled, err, sizeof(err)) != 0) {
        reply_err(c, id, "internal", err);
        return;
    }
    reply_ok(c, id, NULL);
}

static void cmd_history_list(client *c, int id, const cJSON *args)
{
    int total = 0;
    cJSON *items = oma_store_history_list(arg_int(args, "limit", 50),
                                          arg_int(args, "offset", 0), &total);
    cJSON *result;

    if (items == NULL) {
        reply_err(c, id, "io", "cannot read the call history");
        return;
    }
    result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "items", items);
    cJSON_AddNumberToObject(result, "total", total);
    reply_ok(c, id, result);
}

static void cmd_history_clear(client *c, int id, const cJSON *args)
{
    (void)args;
    if (oma_store_history_clear() != 0) {
        reply_err(c, id, "io", "cannot clear the call history");
        return;
    }
    reply_ok(c, id, NULL);
    oma_ipc_broadcast("history.changed", NULL);
}

static void cmd_contacts_list(client *c, int id, const cJSON *args)
{
    cJSON *result = cJSON_CreateObject();

    (void)args;
    cJSON_AddItemToObject(result, "contacts", oma_store_contacts_list());
    reply_ok(c, id, result);
}

static void cmd_contacts_upsert(client *c, int id, const cJSON *args)
{
    oma_contact saved;
    char err[256] = "";
    cJSON *result, *item;

    if (oma_store_contact_upsert(arg_string(args, "id"), arg_string(args, "name"),
                                 arg_string(args, "number"), &saved,
                                 err, sizeof(err)) != 0) {
        reply_err(c, id, "bad_args", err);
        return;
    }

    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", saved.id);
    cJSON_AddStringToObject(item, "name", saved.name);
    cJSON_AddStringToObject(item, "number", saved.number);
    cJSON_AddNumberToObject(item, "created_at", (double)saved.created_at);

    result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "contact", item);
    reply_ok(c, id, result);
    oma_ipc_broadcast("contacts.changed", NULL);
}

static void cmd_contacts_delete(client *c, int id, const cJSON *args)
{
    const char *contact_id = arg_string(args, "id");

    if (contact_id == NULL) {
        reply_err(c, id, "bad_args", "id is required");
        return;
    }
    if (oma_store_contact_delete(contact_id) != 0) {
        reply_err(c, id, "bad_args", "no contact with that id");
        return;
    }
    reply_ok(c, id, NULL);
    oma_ipc_broadcast("contacts.changed", NULL);
}

static void cmd_call(client *c, int id, const cJSON *args)
{
    const char *dest = arg_string(args, "dest");
    char err[256] = "";
    cJSON *result;
    int call_id = -1;

    if (dest == NULL) {
        reply_err(c, id, "bad_args", "dest is required");
        return;
    }
    if (!oma_config_has_account(live_config)) {
        reply_err(c, id, "no_account", "no SIP account is configured");
        return;
    }
    if (!oma_sip_is_registered()) {
        reply_err(c, id, "not_registered", "the account is not registered");
        return;
    }
    if (oma_calls_active()) {
        reply_err(c, id, "busy", "a call is already in progress");
        return;
    }
    if (oma_calls_place(dest, &call_id, err, sizeof(err)) != 0) {
        reply_err(c, id, "internal", err);
        return;
    }

    result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "call_id", call_id);
    reply_ok(c, id, result);
}

static void cmd_answer(client *c, int id, const cJSON *args)
{
    char err[256] = "";

    if (oma_calls_answer(arg_int(args, "call_id", -1), err, sizeof(err)) != 0) {
        reply_err(c, id, "no_incoming", err);
        return;
    }
    reply_ok(c, id, NULL);
}

static void cmd_hangup(client *c, int id, const cJSON *args)
{
    char err[256] = "";

    if (oma_calls_hangup(arg_int(args, "call_id", -1), err, sizeof(err)) != 0) {
        reply_err(c, id, "no_call", err);
        return;
    }
    reply_ok(c, id, NULL);
}

static void cmd_dtmf(client *c, int id, const cJSON *args)
{
    const char *digits = arg_string(args, "digits");
    char err[256] = "";

    if (digits == NULL) {
        reply_err(c, id, "bad_args", "digits are required");
        return;
    }
    if (oma_calls_dtmf(digits, err, sizeof(err)) != 0) {
        reply_err(c, id, "not_connected", err);
        return;
    }
    reply_ok(c, id, NULL);
}

static void cmd_tone(client *c, int id, const cJSON *args)
{
    const char *digits = arg_string(args, "digits");
    char err[256] = "";

    if (digits == NULL) {
        reply_err(c, id, "bad_args", "digits are required");
        return;
    }
    if (oma_calls_keytone(digits, err, sizeof(err)) != 0) {
        reply_err(c, id, "bad_args", err);
        return;
    }
    reply_ok(c, id, NULL);
}

static void cmd_mute(client *c, int id, const cJSON *args)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(args, "enabled");
    bool enabled = item == NULL || cJSON_IsTrue(item);
    char err[256] = "";
    cJSON *result, *data;

    if (oma_calls_set_muted(enabled, err, sizeof(err)) != 0) {
        reply_err(c, id, "not_connected", err);
        return;
    }

    result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "muted", enabled);
    reply_ok(c, id, result);

    data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "muted", enabled);
    oma_ipc_broadcast("call.mute", data);
}

static void cmd_quit(client *c, int id, const cJSON *args)
{
    (void)args;
    quit_requested = true;
    reply_ok(c, id, NULL);
}

typedef void (*cmd_fn)(client *c, int id, const cJSON *args);

static const struct {
    const char *name;
    cmd_fn      fn;
} commands[] = {
    { "hello",            cmd_hello },
    { "status",           cmd_status },
    { "call",             cmd_call },
    { "answer",           cmd_answer },
    { "hangup",           cmd_hangup },
    { "dtmf",             cmd_dtmf },
    { "tone",             cmd_tone },
    { "mute",             cmd_mute },
    { "config.get",       cmd_config_get },
    { "config.set",       cmd_config_set },
    { "register",         cmd_register },
    { "accounts.list",    cmd_accounts_list },
    { "accounts.select",  cmd_accounts_select },
    { "accounts.upsert",  cmd_accounts_upsert },
    { "accounts.delete",  cmd_accounts_delete },
    { "history.list",     cmd_history_list },
    { "history.clear",    cmd_history_clear },
    { "contacts.list",    cmd_contacts_list },
    { "contacts.upsert",  cmd_contacts_upsert },
    { "contacts.delete",  cmd_contacts_delete },
    { "quit",             cmd_quit },
};

static void handle_line(client *c, char *line)
{
    cJSON *root = cJSON_Parse(line);
    const cJSON *cmd, *args;
    int id;
    size_t i;

    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        reply_err(c, 0, "bad_json", "each line must be one JSON object");
        return;
    }

    id = arg_int(root, "id", 0);
    cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        reply_err(c, id, "bad_args", "\"cmd\" is required");
        return;
    }
    args = cJSON_GetObjectItemCaseSensitive(root, "args");

    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(commands[i].name, cmd->valuestring) == 0) {
            commands[i].fn(c, id, args);
            cJSON_Delete(root);
            return;
        }
    }

    reply_err(c, id, "unknown_cmd", cmd->valuestring);
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------- input */

static void client_read(client *c)
{
    for (;;) {
        ssize_t got = read(c->fd, c->in + c->in_len, sizeof(c->in) - c->in_len);
        size_t start;
        char *newline;

        if (got < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            client_close(c);
            return;
        }
        if (got == 0) {
            client_close(c);
            return;
        }
        c->in_len += (size_t)got;

        start = 0;
        while ((newline = memchr(c->in + start, '\n', c->in_len - start)) != NULL) {
            size_t line_len = (size_t)(newline - (c->in + start));

            *newline = '\0';
            if (c->overflowed) {
                /* The tail of a line we already gave up on. */
                c->overflowed = false;
            } else if (line_len > 0) {
                handle_line(c, c->in + start);
                if (c->fd < 0)
                    return;
            }
            start += line_len + 1;
        }

        if (start > 0) {
            memmove(c->in, c->in + start, c->in_len - start);
            c->in_len -= start;
        }

        /* A line longer than the buffer is either a bug or an attack. Answer
         * once, then swallow everything up to the next newline. */
        if (c->in_len == sizeof(c->in)) {
            reply_err(c, 0, "bad_json", "line too long");
            c->in_len = 0;
            c->overflowed = true;
        }
    }
}

static void accept_client(void)
{
    int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    size_t i;

    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            oma_warn("ipc: accept failed: %m");
        return;
    }

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].fd = fd;
            client_count++;
            oma_dbg("ipc: client connected (%zu total)", client_count);
            return;
        }
    }

    oma_warn("ipc: refusing a connection, %d clients already", MAX_CLIENTS);
    close(fd);
}

/* --------------------------------------------------------------- lifecycle */

int oma_ipc_start(char *err, size_t errlen)
{
    struct sockaddr_un addr;
    mode_t saved_umask;
    size_t i;

    for (i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;

    if (oma_path_runtime(socket_path, sizeof(socket_path), "omacall.sock") != 0) {
        snprintf(err, errlen, "cannot resolve the runtime directory: %s",
                 strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (oma_strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path))
            >= sizeof(addr.sun_path)) {
        snprintf(err, errlen, "socket path is too long: %s", socket_path);
        return -1;
    }

    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        snprintf(err, errlen, "cannot create the socket: %s", strerror(errno));
        return -1;
    }

    saved_umask = umask(0177);   /* 0600 on the socket inode */
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int bind_errno = errno;

        umask(saved_umask);
        if (bind_errno == EADDRINUSE) {
            /* Either another daemon is live, or a stale file from a crash.
             * Connecting is the only way to tell the two apart. */
            int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            bool in_use = false;

            if (probe >= 0) {
                in_use = connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0;
                close(probe);
            }
            if (in_use) {
                snprintf(err, errlen, "another omacalld already owns %s", socket_path);
                close(listen_fd);
                listen_fd = -1;
                return -2;
            }
            oma_warn("ipc: removing the stale socket at %s", socket_path);
            unlink(socket_path);
            saved_umask = umask(0177);
            if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                snprintf(err, errlen, "cannot bind %s: %s", socket_path, strerror(errno));
                umask(saved_umask);
                close(listen_fd);
                listen_fd = -1;
                return -1;
            }
        } else {
            snprintf(err, errlen, "cannot bind %s: %s", socket_path, strerror(bind_errno));
            close(listen_fd);
            listen_fd = -1;
            return -1;
        }
    }
    umask(saved_umask);

    if (listen(listen_fd, 8) != 0) {
        snprintf(err, errlen, "cannot listen on %s: %s", socket_path, strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        unlink(socket_path);
        return -1;
    }

    oma_info("ipc: listening on %s", socket_path);
    return 0;
}

void oma_ipc_stop(void)
{
    size_t i;

    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0)
            client_close(&clients[i]);
    }
    client_count = 0;

    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    if (socket_path[0] != '\0')
        unlink(socket_path);
}

size_t oma_ipc_pollfds(struct pollfd *fds, size_t cap)
{
    size_t used = 0;
    size_t i;

    if (cap == 0)
        return 0;
    fds[used].fd = listen_fd;
    fds[used].events = POLLIN;
    fds[used].revents = 0;
    used++;

    for (i = 0; i < MAX_CLIENTS && used < cap; i++) {
        if (clients[i].fd < 0)
            continue;
        fds[used].fd = clients[i].fd;
        /* Past the soft limit the client is not draining what it already
         * asked for; reading more from it would only produce more. Leave
         * POLLIN off until the queue comes back down. */
        fds[used].events = (clients[i].out_len > QUEUE_SOFT_LIMIT ? 0 : POLLIN)
                         | (clients[i].out_len > 0 ? POLLOUT : 0);
        fds[used].revents = 0;
        used++;
    }
    return used;
}

void oma_ipc_dispatch(const struct pollfd *fds, size_t count)
{
    size_t i, j;

    for (i = 0; i < count; i++) {
        if (fds[i].revents == 0)
            continue;

        if (fds[i].fd == listen_fd) {
            if (fds[i].revents & POLLIN)
                accept_client();
            continue;
        }

        for (j = 0; j < MAX_CLIENTS; j++) {
            client *c = &clients[j];

            if (c->fd != fds[i].fd)
                continue;
            if (fds[i].revents & POLLOUT)
                client_flush(c);
            if (c->fd >= 0 && (fds[i].revents & POLLIN))
                client_read(c);
            if (c->fd >= 0 && (fds[i].revents & (POLLHUP | POLLERR)))
                client_close(c);
            if (c->fd < 0 && client_count > 0)
                client_count--;
            break;
        }
    }
}
