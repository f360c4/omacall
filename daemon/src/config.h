#ifndef OMA_CONFIG_H
#define OMA_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

/* Four is the whole v1 codec set: G.729, Opus, PCMA, PCMU. The extra room is
 * for a user who lists them in a different order with a duplicate typo. */
#define OMA_MAX_CODECS 8

/* Saved accounts. Only one is registered at a time — this is a way to keep
 * several servers configured and switch between them, not multi-account. */
#define OMA_MAX_ACCOUNTS 8
#define OMA_ACCOUNT_NAME_MAX 64

typedef struct {
    char display_name[128];
    char username[128];
    char auth_username[128];   /* empty means: authenticate as `username` */
    char password[256];
    char domain[256];
    char proxy[256];           /* empty means: no outbound proxy */
    int  port;                 /* local SIP port; 0 = ephemeral */
    int  register_expires;
    char transport[8];         /* "udp" or "tls" */
} oma_account;

typedef struct {
    /* Whether the server's certificate has to check out. Off is for a lab with
     * a self-signed certificate, and says so in the log — it means anyone able
     * to sit between you and the PBX can read the signalling. */
    bool verify_server;
    /* A CA bundle, when the server's certificate is signed by something the
     * system store does not know. Empty uses the system store. */
    char ca_file[512];
} oma_tls;

typedef struct {
    char   capture[64];
    char   playback[64];
    bool   echo_cancel;
    double ring_volume;
} oma_audio;

typedef struct {
    char stun_server[256];
    int  keepalive_sec;
} oma_nat;

/* One saved account: the SIP side plus the TLS settings, which belong to the
 * server rather than to this machine. */
typedef struct {
    char        name[OMA_ACCOUNT_NAME_MAX];
    oma_account account;
    oma_tls     tls;
} oma_profile;

typedef struct {
    int         version;

    oma_profile accounts[OMA_MAX_ACCOUNTS];
    size_t      account_count;
    char        active_account[OMA_ACCOUNT_NAME_MAX];

    /* The active profile's account and TLS, resolved. The rest of the daemon
     * reads these and never has to care that profiles exist. */
    oma_account account;
    char        codecs[OMA_MAX_CODECS][32];
    size_t      codec_count;
    oma_audio   audio;
    oma_tls     tls;
    oma_nat     nat;
    /* "rfc4733" (out of band telephone-event, the default and what nearly
     * every gateway wants), "rfc2833" (accepted as a synonym, stored as
     * rfc4733), "info" (SIP INFO) or "inband" (audible tones in the audio). */
    char        dtmf_method[16];
    int         ring_timeout_sec;
    int         log_level;
} oma_config;

void oma_config_defaults(oma_config *cfg);

/* Point cfg->account and cfg->tls at the profile named by active_account,
 * falling back to the first one. Call after anything that changes the list. */
void oma_config_apply_active(oma_config *cfg);

/* Index of a profile by name, or -1. */
int  oma_config_find_account(const oma_config *cfg, const char *name);

/* Read one account (and its optional "tls" object) out of a JSON object, so
 * the IPC layer does not have to restate every field name. `json` is a
 * `const cJSON *`, declared void to keep cJSON out of callers that do not
 * already include it. */
void oma_config_take_account_json(const void *json, oma_account *account, oma_tls *tls);

/* Read `path`. A missing file is not an error: the caller gets defaults and an
 * account that oma_config_has_account() rejects, which is how a fresh install
 * behaves. Returns 0 on success, -1 with a message in `err`. */
int oma_config_load(oma_config *cfg, const char *path, char *err, size_t errlen);

/* Write `path` with mode 0600, atomically. */
int oma_config_save(const oma_config *cfg, const char *path);

/* Serialize to JSON. With `redact`, "password" is replaced by
 * "password_set": true|false — that is what config.get returns, so a password
 * never crosses the socket. Caller frees. */
char *oma_config_to_json(const oma_config *cfg, bool redact);

/* Shallow per-section merge of a partial config object, as config.set sends. */
int oma_config_merge_json(oma_config *cfg, const char *json, char *err, size_t errlen);

/* True once there is enough to attempt a REGISTER. */
bool oma_config_has_account(const oma_config *cfg);

#endif /* OMA_CONFIG_H */
