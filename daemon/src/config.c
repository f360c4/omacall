#include "config.h"

#include "log.h"
#include "util.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const default_codecs[] = {
    "G729/8000", "opus/48000", "PCMA/8000", "PCMU/8000"
};

void oma_config_defaults(oma_config *cfg)
{
    size_t i;

    memset(cfg, 0, sizeof(*cfg));
    cfg->version = 1;
    cfg->account.port = 5060;
    cfg->account.register_expires = 300;
    oma_strlcpy(cfg->account.transport, "udp", sizeof(cfg->account.transport));
    cfg->tls.verify_server = true;

    /* One empty profile, so a fresh install has somewhere to put an account
     * without the panel having to invent a list first. */
    oma_strlcpy(cfg->accounts[0].name, "Default", sizeof(cfg->accounts[0].name));
    cfg->accounts[0].account = cfg->account;
    cfg->accounts[0].tls = cfg->tls;
    cfg->account_count = 1;
    oma_strlcpy(cfg->active_account, "Default", sizeof(cfg->active_account));

    for (i = 0; i < sizeof(default_codecs) / sizeof(default_codecs[0]); i++)
        oma_strlcpy(cfg->codecs[i], default_codecs[i], sizeof(cfg->codecs[i]));
    cfg->codec_count = sizeof(default_codecs) / sizeof(default_codecs[0]);

    oma_strlcpy(cfg->audio.capture, "default", sizeof(cfg->audio.capture));
    oma_strlcpy(cfg->audio.playback, "default", sizeof(cfg->audio.playback));
    cfg->audio.echo_cancel = true;
    cfg->audio.ring_volume = 0.8;

    cfg->nat.keepalive_sec = 15;
    oma_strlcpy(cfg->dtmf_method, "rfc4733", sizeof(cfg->dtmf_method));
    cfg->ring_timeout_sec = 45;
    cfg->log_level = 3;
}

static void take_string(const cJSON *obj, const char *key, char *dst, size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsString(item) && item->valuestring != NULL)
        oma_strlcpy(dst, item->valuestring, len);
}

static void take_int(const cJSON *obj, const char *key, int *dst, int min, int max)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsNumber(item))
        return;
    if (item->valueint < min || item->valueint > max) {
        oma_warn("config: %s = %d is out of range [%d, %d], keeping %d",
                 key, item->valueint, min, max, *dst);
        return;
    }
    *dst = item->valueint;
}

static void take_bool(const cJSON *obj, const char *key, bool *dst)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (cJSON_IsBool(item))
        *dst = cJSON_IsTrue(item) ? true : false;
}

static void take_double(const cJSON *obj, const char *key, double *dst,
                        double min, double max)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsNumber(item))
        return;
    if (item->valuedouble < min || item->valuedouble > max) {
        oma_warn("config: %s = %f is out of range, keeping %f",
                 key, item->valuedouble, *dst);
        return;
    }
    *dst = item->valuedouble;
}

static void take_codecs(const cJSON *root, oma_config *cfg)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "codecs");
    const cJSON *item;
    size_t count = 0;

    if (!cJSON_IsArray(array))
        return;

    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item) || item->valuestring == NULL)
            continue;
        if (count >= OMA_MAX_CODECS) {
            oma_warn("config: more than %d codecs listed, ignoring the rest",
                     OMA_MAX_CODECS);
            break;
        }
        oma_strlcpy(cfg->codecs[count], item->valuestring, sizeof(cfg->codecs[count]));
        count++;
    }

    /* An empty list would leave the account unable to negotiate anything, so
     * treat it as "not configured" and keep the defaults. */
    if (count > 0)
        cfg->codec_count = count;
    else
        oma_warn("config: \"codecs\" is empty, keeping the default order");
}

/* Fill one account from a JSON object. Shared by the legacy singular
 * "account" and by each entry of "accounts". */
static void take_account(const cJSON *section, oma_account *a)
{
    {
        take_string(section, "display_name", a->display_name, sizeof(a->display_name));
        take_string(section, "username", a->username, sizeof(a->username));
        take_string(section, "auth_username", a->auth_username, sizeof(a->auth_username));
        take_string(section, "password", a->password, sizeof(a->password));
        take_string(section, "domain", a->domain, sizeof(a->domain));
        take_string(section, "proxy", a->proxy, sizeof(a->proxy));
        take_int(section, "port", &a->port, 0, 65535);
        take_int(section, "register_expires", &a->register_expires, 30, 86400);
        take_string(section, "transport", a->transport, sizeof(a->transport));
        if (strcmp(a->transport, "udp") != 0 && strcmp(a->transport, "tls") != 0) {
            oma_warn("config: transport \"%s\" is not udp or tls, using udp",
                     a->transport);
            oma_strlcpy(a->transport, "udp", sizeof(a->transport));
        }
    }
}

static void take_tls(const cJSON *section, oma_tls *tls)
{
    if (!cJSON_IsObject(section))
        return;
    take_bool(section, "verify_server", &tls->verify_server);
    take_string(section, "ca_file", tls->ca_file, sizeof(tls->ca_file));
}

/* The saved accounts, plus the legacy singular form.
 *
 * A config written before profiles existed has one "account" object and no
 * list. Reading it as a profile called Default means nobody has to edit a file
 * by hand to keep what they had. */
static void take_accounts(const cJSON *root, oma_config *cfg)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "accounts");
    const cJSON *legacy = cJSON_GetObjectItemCaseSensitive(root, "account");
    const cJSON *item;

    cfg->account_count = 0;

    cJSON_ArrayForEach(item, array) {
        oma_profile *slot;
        const cJSON *name;

        if (!cJSON_IsObject(item))
            continue;
        if (cfg->account_count >= OMA_MAX_ACCOUNTS) {
            oma_warn("config: more than %d accounts, ignoring the rest",
                     OMA_MAX_ACCOUNTS);
            break;
        }

        slot = &cfg->accounts[cfg->account_count];
        memset(slot, 0, sizeof(*slot));
        slot->account.port = 5060;
        slot->account.register_expires = 300;
        oma_strlcpy(slot->account.transport, "udp", sizeof(slot->account.transport));
        slot->tls.verify_server = true;

        name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (cJSON_IsString(name) && name->valuestring[0] != '\0')
            oma_strlcpy(slot->name, name->valuestring, sizeof(slot->name));
        else
            snprintf(slot->name, sizeof(slot->name), "Account %zu",
                     cfg->account_count + 1);

        take_account(item, &slot->account);
        take_tls(cJSON_GetObjectItemCaseSensitive(item, "tls"), &slot->tls);
        cfg->account_count++;
    }

    if (cfg->account_count == 0 && cJSON_IsObject(legacy)) {
        oma_profile *slot = &cfg->accounts[0];

        memset(slot, 0, sizeof(*slot));
        slot->account.port = 5060;
        slot->account.register_expires = 300;
        oma_strlcpy(slot->account.transport, "udp", sizeof(slot->account.transport));
        slot->tls.verify_server = true;
        oma_strlcpy(slot->name, "Default", sizeof(slot->name));

        take_account(legacy, &slot->account);
        take_tls(cJSON_GetObjectItemCaseSensitive(root, "tls"), &slot->tls);
        cfg->account_count = 1;
        oma_info("config: migrated the single account into a profile named Default");
    }

    take_string(root, "active_account", cfg->active_account,
                sizeof(cfg->active_account));
}

/* `full` is a config file being read; otherwise this is a partial patch from
 * config.set, where the absence of a key means "leave it alone" rather than
 * "empty". The difference matters most for the account list: a patch that
 * carries only "account" is editing the active profile, not replacing every
 * profile with one called Default. */
static void apply_object(oma_config *cfg, const cJSON *root, bool full)
{
    const cJSON *section;

    take_int(root, "version", &cfg->version, 1, 1);

    if (full || cJSON_GetObjectItemCaseSensitive(root, "accounts") != NULL) {
        take_accounts(root, cfg);
    } else {
        /* Editing the account in place: apply to the active profile and let
         * apply_active copy it back out. */
        int index = oma_config_find_account(cfg, cfg->active_account);

        if (index >= 0) {
            section = cJSON_GetObjectItemCaseSensitive(root, "account");
            if (cJSON_IsObject(section))
                take_account(section, &cfg->accounts[index].account);
            take_tls(cJSON_GetObjectItemCaseSensitive(root, "tls"),
                     &cfg->accounts[index].tls);
        }
        take_string(root, "active_account", cfg->active_account,
                    sizeof(cfg->active_account));
    }

    take_codecs(root, cfg);

    section = cJSON_GetObjectItemCaseSensitive(root, "audio");
    if (cJSON_IsObject(section)) {
        oma_audio *au = &cfg->audio;
        take_string(section, "capture", au->capture, sizeof(au->capture));
        take_string(section, "playback", au->playback, sizeof(au->playback));
        take_bool(section, "echo_cancel", &au->echo_cancel);
        take_double(section, "ring_volume", &au->ring_volume, 0.0, 1.0);
    }

    section = cJSON_GetObjectItemCaseSensitive(root, "nat");
    if (cJSON_IsObject(section)) {
        oma_nat *nat = &cfg->nat;
        take_string(section, "stun_server", nat->stun_server, sizeof(nat->stun_server));
        take_int(section, "keepalive_sec", &nat->keepalive_sec, 0, 3600);
    }

    take_string(root, "dtmf_method", cfg->dtmf_method, sizeof(cfg->dtmf_method));
    /* rfc2833 and rfc4733 name the same thing: RFC 4733 is the revision that
     * obsoleted RFC 2833, and both describe telephone-event payloads. pjsip
     * still calls the method RFC2833. Accept either spelling so a config
     * written from either document works. */
    if (strcmp(cfg->dtmf_method, "rfc2833") == 0)
        oma_strlcpy(cfg->dtmf_method, "rfc4733", sizeof(cfg->dtmf_method));
    if (strcmp(cfg->dtmf_method, "rfc4733") != 0
            && strcmp(cfg->dtmf_method, "info") != 0
            && strcmp(cfg->dtmf_method, "inband") != 0) {
        oma_warn("config: dtmf_method \"%s\" is not rfc4733, rfc2833, info or "
                 "inband; using rfc4733", cfg->dtmf_method);
        oma_strlcpy(cfg->dtmf_method, "rfc4733", sizeof(cfg->dtmf_method));
    }
    take_int(root, "ring_timeout_sec", &cfg->ring_timeout_sec, 5, 600);
    take_int(root, "log_level", &cfg->log_level, 0, 6);

    oma_config_apply_active(cfg);
}

void oma_config_take_account_json(const void *json, oma_account *account, oma_tls *tls)
{
    const cJSON *root = json;

    take_account(root, account);
    take_tls(cJSON_GetObjectItemCaseSensitive(root, "tls"), tls);
}

int oma_config_find_account(const oma_config *cfg, const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0')
        return -1;
    for (i = 0; i < cfg->account_count; i++) {
        if (strcmp(cfg->accounts[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

void oma_config_apply_active(oma_config *cfg)
{
    int index;

    if (cfg->account_count == 0) {
        memset(&cfg->account, 0, sizeof(cfg->account));
        cfg->account.port = 5060;
        cfg->account.register_expires = 300;
        oma_strlcpy(cfg->account.transport, "udp", sizeof(cfg->account.transport));
        cfg->tls.verify_server = true;
        cfg->active_account[0] = '\0';
        return;
    }

    index = oma_config_find_account(cfg, cfg->active_account);
    if (index < 0) {
        /* No selection, or one naming an account that is gone: the first is
         * the sensible answer, and saying so beats registering nothing. */
        if (cfg->active_account[0] != '\0')
            oma_warn("config: no account named \"%s\", using \"%s\"",
                     cfg->active_account, cfg->accounts[0].name);
        index = 0;
        oma_strlcpy(cfg->active_account, cfg->accounts[0].name,
                    sizeof(cfg->active_account));
    }

    cfg->account = cfg->accounts[index].account;
    cfg->tls = cfg->accounts[index].tls;
}

int oma_config_load(oma_config *cfg, const char *path, char *err, size_t errlen)
{
    char *text;
    cJSON *root;

    oma_config_defaults(cfg);

    text = oma_read_file(path, NULL);
    if (text == NULL) {
        if (errno == ENOENT) {
            oma_info("config: %s does not exist yet, starting unconfigured", path);
            return 0;
        }
        snprintf(err, errlen, "cannot read %s: %s", path, strerror(errno));
        return -1;
    }

    root = cJSON_Parse(text);
    free(text);
    if (root == NULL) {
        snprintf(err, errlen, "%s is not valid JSON", path);
        return -1;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(err, errlen, "%s must contain a JSON object", path);
        return -1;
    }

    apply_object(cfg, root, true);
    cJSON_Delete(root);
    return 0;
}

int oma_config_merge_json(oma_config *cfg, const char *json, char *err, size_t errlen)
{
    cJSON *root = cJSON_Parse(json);

    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(err, errlen, "expected a JSON object");
        return -1;
    }
    apply_object(cfg, root, false);
    cJSON_Delete(root);
    return 0;
}

/* One account as JSON. With `redact` the password is replaced by a flag, which
 * is what crosses the socket: a client never needs to read a password back. */
static void put_account(cJSON *target, const oma_account *a, bool redact)
{
    cJSON_AddStringToObject(target, "display_name", a->display_name);
    cJSON_AddStringToObject(target, "username", a->username);
    cJSON_AddStringToObject(target, "auth_username", a->auth_username);
    if (redact)
        cJSON_AddBoolToObject(target, "password_set", a->password[0] != '\0');
    else
        cJSON_AddStringToObject(target, "password", a->password);
    cJSON_AddStringToObject(target, "domain", a->domain);
    cJSON_AddStringToObject(target, "proxy", a->proxy);
    cJSON_AddNumberToObject(target, "port", a->port);
    cJSON_AddNumberToObject(target, "register_expires", a->register_expires);
    cJSON_AddStringToObject(target, "transport", a->transport);
}

static void put_tls(cJSON *target, const oma_tls *tls)
{
    cJSON *obj = cJSON_AddObjectToObject(target, "tls");

    cJSON_AddBoolToObject(obj, "verify_server", tls->verify_server);
    cJSON_AddStringToObject(obj, "ca_file", tls->ca_file);
}

char *oma_config_to_json(const oma_config *cfg, bool redact)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *accounts, *account, *codecs, *audio, *nat;
    size_t i;
    char *text;

    if (root == NULL)
        return NULL;

    cJSON_AddNumberToObject(root, "version", cfg->version);

    accounts = cJSON_AddArrayToObject(root, "accounts");
    for (i = 0; i < cfg->account_count; i++) {
        cJSON *entry = cJSON_CreateObject();

        cJSON_AddStringToObject(entry, "name", cfg->accounts[i].name);
        put_account(entry, &cfg->accounts[i].account, redact);
        put_tls(entry, &cfg->accounts[i].tls);
        cJSON_AddItemToArray(accounts, entry);
    }
    cJSON_AddStringToObject(root, "active_account", cfg->active_account);

    /* The resolved active account, so a client that only cares about "the
     * account" — which is most of them — does not have to walk the list. */
    account = cJSON_AddObjectToObject(root, "account");
    put_account(account, &cfg->account, redact);
    put_tls(root, &cfg->tls);

    codecs = cJSON_AddArrayToObject(root, "codecs");
    for (i = 0; i < cfg->codec_count; i++)
        cJSON_AddItemToArray(codecs, cJSON_CreateString(cfg->codecs[i]));

    audio = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddStringToObject(audio, "capture", cfg->audio.capture);
    cJSON_AddStringToObject(audio, "playback", cfg->audio.playback);
    cJSON_AddBoolToObject(audio, "echo_cancel", cfg->audio.echo_cancel);
    cJSON_AddNumberToObject(audio, "ring_volume", cfg->audio.ring_volume);

    nat = cJSON_AddObjectToObject(root, "nat");
    cJSON_AddStringToObject(nat, "stun_server", cfg->nat.stun_server);
    cJSON_AddNumberToObject(nat, "keepalive_sec", cfg->nat.keepalive_sec);

    cJSON_AddStringToObject(root, "dtmf_method", cfg->dtmf_method);
    cJSON_AddNumberToObject(root, "ring_timeout_sec", cfg->ring_timeout_sec);
    cJSON_AddNumberToObject(root, "log_level", cfg->log_level);

    text = redact ? cJSON_PrintUnformatted(root) : cJSON_Print(root);
    cJSON_Delete(root);
    return text;
}

int oma_config_save(const oma_config *cfg, const char *path)
{
    char *text = oma_config_to_json(cfg, false);
    int rc;

    if (text == NULL)
        return -1;
    rc = oma_write_atomic(path, text, strlen(text), 0600);
    free(text);
    return rc;
}

bool oma_config_has_account(const oma_config *cfg)
{
    return cfg->account.username[0] != '\0' && cfg->account.domain[0] != '\0';
}
