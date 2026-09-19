#include "calls.h"

#include "ipc.h"
#include "log.h"
#include "store.h"
#include "tones.h"
#include "util.h"

#include <pjsua-lib/pjsua.h>

#include <stdio.h>
#include <string.h>

static oma_call current = { .id = -1 };
static oma_config settings;
static bool have_call;

void oma_calls_init(const oma_config *cfg)
{
    settings = *cfg;
    current.id = -1;
    have_call = false;
}

void oma_calls_shutdown(void)
{
    if (have_call && current.id >= 0)
        pjsua_call_hangup((pjsua_call_id)current.id, 0, NULL, NULL);
    have_call = false;
    current.id = -1;
}

bool oma_calls_active(void)
{
    return have_call;
}

const oma_call *oma_calls_current(void)
{
    return have_call ? &current : NULL;
}

cJSON *oma_calls_json(void)
{
    cJSON *obj;

    if (!have_call)
        return cJSON_CreateNull();

    obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "call_id", current.id);
    cJSON_AddStringToObject(obj, "direction", current.direction);
    cJSON_AddStringToObject(obj, "state", oma_state_name(current.state));
    cJSON_AddStringToObject(obj, "remote", current.remote);
    cJSON_AddStringToObject(obj, "number", current.number);
    cJSON_AddStringToObject(obj, "display", current.display);
    cJSON_AddStringToObject(obj, "codec", current.codec);
    cJSON_AddBoolToObject(obj, "muted", current.muted);
    if (current.connected_at > 0)
        cJSON_AddNumberToObject(obj, "started_at", (double)current.connected_at);
    else
        cJSON_AddNullToObject(obj, "started_at");
    return obj;
}

/* "1002" -> "sip:1002@domain"; anything already a URI is left alone.
 *
 * On TLS the target needs ;transport=tls just as much as the account's own
 * URIs do. Without it pjsip resolves the destination the usual way, finds no
 * UDP transport open and refuses the call with PJSIP_EUNSUPTRANSPORT — which
 * reads as "this build has no transport" rather than "this URI asked for the
 * wrong one". */
static int build_uri(const char *dest, char *out, size_t len, char *err, size_t errlen)
{
    bool tls = strcmp(settings.account.transport, "tls") == 0;
    const char *suffix;
    int n;

    if (dest == NULL || dest[0] == '\0') {
        snprintf(err, errlen, "a destination is required");
        return -1;
    }

    if (strncasecmp(dest, "sip:", 4) == 0 || strncasecmp(dest, "sips:", 5) == 0) {
        /* A URI the caller wrote by hand keeps whatever transport it names. */
        suffix = (tls && strstr(dest, "transport=") == NULL) ? ";transport=tls" : "";
        n = snprintf(out, len, "%s%s", dest, suffix);
    } else if (settings.account.domain[0] == '\0') {
        snprintf(err, errlen, "no SIP domain is configured");
        return -1;
    } else {
        suffix = tls ? ";transport=tls" : "";
        n = snprintf(out, len, "sip:%s@%s%s", dest, settings.account.domain, suffix);
    }

    if (n < 0 || (size_t)n >= len) {
        snprintf(err, errlen, "the destination is too long");
        return -1;
    }
    return 0;
}

/* Pull "1002" and "Fulano" out of "\"Fulano\" <sip:1002@host>". */
static void split_uri(const char *uri, char *number, size_t number_len,
                      char *display, size_t display_len)
{
    const char *user_start, *at, *angle;
    size_t len;

    number[0] = '\0';
    display[0] = '\0';

    angle = strchr(uri, '<');
    if (angle != NULL && angle != uri) {
        const char *end = angle - 1;
        const char *start = uri;

        while (*start == ' ' || *start == '"')
            start++;
        while (end > start && (*end == ' ' || *end == '"'))
            end--;
        if (end >= start) {
            len = (size_t)(end - start) + 1;
            if (len >= display_len)
                len = display_len - 1;
            memcpy(display, start, len);
            display[len] = '\0';
        }
    }

    user_start = strstr(uri, "sip:");
    if (user_start == NULL)
        user_start = uri;
    else
        user_start += 4;

    at = strchr(user_start, '@');
    if (at == NULL)
        at = strpbrk(user_start, ">;");
    if (at == NULL)
        at = user_start + strlen(user_start);

    len = (size_t)(at - user_start);
    if (len >= number_len)
        len = number_len - 1;
    memcpy(number, user_start, len);
    number[len] = '\0';
}

static void begin_call(int call_id, const char *direction, const char *remote)
{
    const char *known;

    memset(&current, 0, sizeof(current));
    current.id = call_id;
    oma_strlcpy(current.direction, direction, sizeof(current.direction));
    oma_strlcpy(current.remote, remote, sizeof(current.remote));
    split_uri(remote, current.number, sizeof(current.number),
              current.display, sizeof(current.display));

    /* A saved contact's name beats whatever the far end put in its From
     * header, which is often the raw number or the carrier's own label. */
    known = oma_store_contact_name_for_number(current.number);
    if (known != NULL)
        oma_strlcpy(current.display, known, sizeof(current.display));

    current.created_at = oma_now();
    current.state = strcmp(direction, "out") == 0 ? OMA_STATE_CALLING
                                                  : OMA_STATE_INCOMING;
    have_call = true;
    oma_sip_note_call_state(true, current.state);
}

int oma_calls_place(const char *dest, int *call_id, char *err, size_t errlen)
{
    char uri[512];
    pj_str_t target;
    pjsua_call_id id;
    pj_status_t status;
    int account = oma_sip_account_id();

    if (account < 0) {
        snprintf(err, errlen, "no SIP account is configured");
        return -1;
    }
    if (!oma_sip_is_registered()) {
        snprintf(err, errlen, "the account is not registered");
        return -1;
    }
    if (have_call) {
        snprintf(err, errlen, "a call is already in progress");
        return -1;
    }
    if (build_uri(dest, uri, sizeof(uri), err, errlen) != 0)
        return -1;

    target = pj_str(uri);
    status = pjsua_call_make_call((pjsua_acc_id)account, &target, NULL, NULL, NULL, &id);
    if (status != PJ_SUCCESS) {
        char reason[PJ_ERR_MSG_SIZE];

        pj_strerror(status, reason, sizeof(reason));
        snprintf(err, errlen, "cannot place the call: %s", reason);
        return -1;
    }

    begin_call((int)id, "out", uri);
    if (call_id != NULL)
        *call_id = (int)id;
    oma_info("call %d: calling %s", (int)id, uri);
    return 0;
}

int oma_calls_answer(int call_id, char *err, size_t errlen)
{
    if (!have_call || current.state != OMA_STATE_INCOMING) {
        snprintf(err, errlen, "there is no incoming call");
        return -1;
    }
    if (call_id >= 0 && call_id != current.id) {
        snprintf(err, errlen, "there is no incoming call with that id");
        return -1;
    }
    if (pjsua_call_answer((pjsua_call_id)current.id, 200, NULL, NULL) != PJ_SUCCESS) {
        snprintf(err, errlen, "cannot answer the call");
        return -1;
    }
    oma_tones_stop();
    return 0;
}

int oma_calls_hangup(int call_id, char *err, size_t errlen)
{
    unsigned code;

    if (!have_call) {
        snprintf(err, errlen, "there is no call");
        return -1;
    }
    if (call_id >= 0 && call_id != current.id) {
        snprintf(err, errlen, "there is no call with that id");
        return -1;
    }

    /* Rejecting a call that is still ringing is 486 Busy Here; ending one that
     * was answered, or cancelling one we placed, is a plain BYE/CANCEL. */
    code = current.state == OMA_STATE_INCOMING ? 486 : 0;

    if (pjsua_call_hangup((pjsua_call_id)current.id, code, NULL, NULL) != PJ_SUCCESS) {
        snprintf(err, errlen, "cannot hang up");
        return -1;
    }
    current.state = OMA_STATE_DISCONNECTING;
    oma_sip_note_call_state(true, current.state);
    return 0;
}

int oma_calls_dtmf(const char *digits, char *err, size_t errlen)
{
    pj_str_t payload;
    size_t i;

    if (!have_call || current.state != OMA_STATE_CONNECTED) {
        snprintf(err, errlen, "no call is connected");
        return -1;
    }
    if (digits == NULL || digits[0] == '\0') {
        snprintf(err, errlen, "digits are required");
        return -1;
    }
    for (i = 0; digits[i] != '\0'; i++) {
        char d = digits[i];

        if (!((d >= '0' && d <= '9') || d == '*' || d == '#'
              || (d >= 'A' && d <= 'D') || (d >= 'a' && d <= 'd'))) {
            snprintf(err, errlen, "'%c' is not a valid DTMF digit", d);
            return -1;
        }
    }

    if (strcmp(settings.dtmf_method, "inband") == 0) {
        pjsua_call_info info;

        if (pjsua_call_get_info((pjsua_call_id)current.id, &info) != PJ_SUCCESS
                || info.conf_slot == PJSUA_INVALID_ID) {
            snprintf(err, errlen, "the call has no media yet");
            return -1;
        }
        if (oma_tones_dtmf(digits, (int)info.conf_slot) != 0) {
            snprintf(err, errlen, "cannot play inband DTMF");
            return -1;
        }
        oma_info("call %d: sent DTMF %s inband", current.id, digits);
        return 0;
    }

    payload = pj_str((char *)digits);

    if (strcmp(settings.dtmf_method, "info") == 0) {
        pjsua_call_send_dtmf_param param;

        pjsua_call_send_dtmf_param_default(&param);
        param.method = PJSUA_DTMF_METHOD_SIP_INFO;
        param.digits = payload;
        if (pjsua_call_send_dtmf((pjsua_call_id)current.id, &param) != PJ_SUCCESS) {
            snprintf(err, errlen, "cannot send DTMF over SIP INFO");
            return -1;
        }
    } else if (pjsua_call_dial_dtmf((pjsua_call_id)current.id, &payload) != PJ_SUCCESS) {
        snprintf(err, errlen, "cannot send DTMF");
        return -1;
    }

    /* The digit left over the signalling path, which makes no sound on the
     * line. Every desk phone still clicks back at you, so play the tone to our
     * own speaker — otherwise pressing a key feels like nothing happened. */
    oma_tones_dtmf(digits, OMA_TONES_LOCAL_ONLY);

    oma_info("call %d: sent DTMF %s (%s)", current.id, digits,
             settings.dtmf_method);
    return 0;
}

int oma_calls_keytone(const char *digits, char *err, size_t errlen)
{
    size_t i;

    if (digits == NULL || digits[0] == '\0') {
        snprintf(err, errlen, "digits are required");
        return -1;
    }
    for (i = 0; digits[i] != '\0'; i++) {
        char d = digits[i];

        if (!((d >= '0' && d <= '9') || d == '*' || d == '#'
              || (d >= 'A' && d <= 'D') || (d >= 'a' && d <= 'd'))) {
            snprintf(err, errlen, "'%c' is not a valid DTMF digit", d);
            return -1;
        }
    }

    if (oma_tones_dtmf(digits, OMA_TONES_LOCAL_ONLY) != 0) {
        snprintf(err, errlen, "cannot play the tone");
        return -1;
    }
    return 0;
}

int oma_calls_set_muted(bool muted, char *err, size_t errlen)
{
    pjsua_call_info info;

    if (!have_call || current.state != OMA_STATE_CONNECTED) {
        snprintf(err, errlen, "no call is connected");
        return -1;
    }
    if (pjsua_call_get_info((pjsua_call_id)current.id, &info) != PJ_SUCCESS) {
        snprintf(err, errlen, "cannot read the call state");
        return -1;
    }
    if (info.conf_slot == PJSUA_INVALID_ID) {
        snprintf(err, errlen, "the call has no media yet");
        return -1;
    }

    /* Mute is the microphone leg only: slot 0 (the sound device) stops feeding
     * the call, while the call keeps feeding slot 0 so we still hear them. */
    if (muted)
        pjsua_conf_disconnect(0, info.conf_slot);
    else if (pjsua_conf_connect(0, info.conf_slot) != PJ_SUCCESS) {
        snprintf(err, errlen, "cannot unmute");
        return -1;
    }

    current.muted = muted;
    return 0;
}

static void finish_call(const oma_event *ev)
{
    oma_history_item item;
    cJSON *data;
    int duration = 0;

    if (!have_call)
        return;

    if (current.connected_at > 0)
        duration = (int)(oma_now() - current.connected_at);

    memset(&item, 0, sizeof(item));
    item.ts = current.created_at;
    oma_strlcpy(item.direction, current.direction, sizeof(item.direction));
    oma_strlcpy(item.number, current.number, sizeof(item.number));
    oma_strlcpy(item.display, current.display, sizeof(item.display));
    oma_strlcpy(item.reason, ev->reason, sizeof(item.reason));
    item.duration_sec = duration;
    item.status_code = ev->status_code;

    if (oma_store_history_append(&item) == 0)
        oma_store_history_trim();

    data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "call_id", current.id);
    cJSON_AddStringToObject(data, "direction", item.direction);
    cJSON_AddStringToObject(data, "number", item.number);
    cJSON_AddStringToObject(data, "display", item.display);
    cJSON_AddStringToObject(data, "reason", item.reason);
    cJSON_AddNumberToObject(data, "status_code", ev->status_code);
    cJSON_AddNumberToObject(data, "duration_sec", duration);
    cJSON_AddNumberToObject(data, "ts", (double)oma_now());

    oma_info("call %d: ended (%s, %d %s) after %ds", current.id, item.reason,
             ev->status_code, ev->status_text, duration);

    have_call = false;
    current.id = -1;
    oma_tones_stop();
    oma_sip_note_call_state(false, OMA_STATE_IDLE);

    oma_ipc_broadcast("call.ended", data);
    oma_ipc_broadcast("history.changed", NULL);
}

void oma_calls_handle_event(const oma_event *ev)
{
    switch (ev->type) {
    case OMA_EV_CALL_INCOMING:
        /* A second call while one is up was already rejected in the callback;
         * only a genuinely new call reaches here. */
        begin_call(ev->call_id, "in", ev->remote);
        oma_tones_ring();
        {
            cJSON *data = cJSON_CreateObject();

            cJSON_AddNumberToObject(data, "call_id", current.id);
            cJSON_AddStringToObject(data, "remote", current.remote);
            cJSON_AddStringToObject(data, "display", current.display);
            cJSON_AddStringToObject(data, "number", current.number);
            cJSON_AddStringToObject(data, "contact_name", current.display);
            cJSON_AddNumberToObject(data, "ts", (double)current.created_at);
            oma_ipc_broadcast("call.incoming", data);
        }
        oma_info("call %d: incoming from %s", current.id, current.remote);
        break;

    case OMA_EV_CALL_STATE: {
        cJSON *data;

        if (!have_call || ev->call_id != current.id)
            break;

        if (strcmp(ev->state, "early") == 0) {
            current.state = OMA_STATE_EARLY;
            /* Only ring locally when the far end is not already sending us
             * its own ringback; otherwise the user hears both at once. */
            if (!current.media_active && strcmp(current.direction, "out") == 0)
                oma_tones_ringback();
        } else if (strcmp(ev->state, "connected") == 0) {
            current.state = OMA_STATE_CONNECTED;
            if (current.connected_at == 0)
                current.connected_at = oma_now();
            oma_tones_stop();
        } else if (strcmp(ev->state, "disconnecting") == 0) {
            current.state = OMA_STATE_DISCONNECTING;
        } else if (strcmp(ev->state, "calling") == 0) {
            current.state = OMA_STATE_CALLING;
        }
        oma_sip_note_call_state(true, current.state);

        data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "call_id", current.id);
        cJSON_AddStringToObject(data, "state", oma_state_name(current.state));
        cJSON_AddStringToObject(data, "remote", current.remote);
        cJSON_AddStringToObject(data, "number", current.number);
        cJSON_AddStringToObject(data, "display", current.display);
        if (current.connected_at > 0)
            cJSON_AddNumberToObject(data, "started_at", (double)current.connected_at);
        else
            cJSON_AddNullToObject(data, "started_at");
        oma_ipc_broadcast("call.state", data);
        break;
    }

    case OMA_EV_MEDIA_STATE: {
        cJSON *data;

        if (!have_call || ev->call_id != current.id)
            break;
        current.media_active = ev->active;
        oma_strlcpy(current.codec, ev->codec, sizeof(current.codec));
        if (ev->active)
            oma_tones_stop();

        data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "call_id", current.id);
        cJSON_AddBoolToObject(data, "active", ev->active);
        cJSON_AddStringToObject(data, "codec", current.codec);
        oma_ipc_broadcast("media.state", data);
        if (ev->active)
            oma_info("call %d: media active, codec %s", current.id, current.codec);
        break;
    }

    case OMA_EV_CALL_ENDED:
        if (have_call && ev->call_id == current.id)
            finish_call(ev);
        break;

    case OMA_EV_REG_STATE:
    case OMA_EV_ERROR:
        break;
    }
}

void oma_calls_tick(void)
{
    int64_t waited;

    /* Inband digits finish on their own schedule; unhook the generator once
     * they have. Cheap enough to check every cycle. */
    oma_tones_dtmf_poll();

    if (!have_call)
        return;
    if (current.state != OMA_STATE_INCOMING && current.state != OMA_STATE_CALLING
            && current.state != OMA_STATE_EARLY)
        return;

    waited = oma_now() - current.created_at;
    if (waited < settings.ring_timeout_sec)
        return;

    oma_info("call %d: no answer after %ds, giving up", current.id,
             settings.ring_timeout_sec);
    /* 408 on the way out reads as "no answer" to the far end, and the
     * disconnect that follows writes the history entry. */
    pjsua_call_hangup((pjsua_call_id)current.id,
                      current.state == OMA_STATE_INCOMING ? 480 : 0, NULL, NULL);
}
