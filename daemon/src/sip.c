#include "sip.h"

#include "calls.h"
#include "events.h"
#include "log.h"
#include "tones.h"
#include "util.h"

#include <pjsua-lib/pjsua.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define THIS_FILE "omacall"

static pjsua_acc_id account_id = PJSUA_INVALID_ID;
static oma_state reg_state = OMA_STATE_IDLE;
static oma_state call_state = OMA_STATE_IDLE;
static bool in_call;

/* Read from a pjsip thread in on_incoming_call and written by the main loop,
 * so it is the one piece of shared state here that has to be atomic. */
static atomic_bool busy_with_a_call;

/* Which transports are open. Switching to an account that uses the other one
 * has to open it first: a transport is created once and lives for the life of
 * the process, and an account whose URI names a transport that does not exist
 * is refused with PJSIP_EINVALIDURI. */
static bool udp_transport_open;
static bool tls_transport_open;
static bool registered;
static int  reg_status_code;
static char reg_status_text[128];
static char account_uri[256];
static oma_config active_config;
static bool pjsua_up;

const char *oma_state_name(oma_state state)
{
    switch (state) {
    case OMA_STATE_IDLE:          return "idle";
    case OMA_STATE_REGISTERING:   return "registering";
    case OMA_STATE_REGISTERED:    return "registered";
    case OMA_STATE_REG_FAILED:    return "reg_failed";
    case OMA_STATE_INCOMING:      return "incoming";
    case OMA_STATE_CALLING:       return "calling";
    case OMA_STATE_EARLY:         return "early";
    case OMA_STATE_CONNECTED:     return "connected";
    case OMA_STATE_DISCONNECTING: return "disconnecting";
    }
    return "idle";
}

oma_state oma_sip_state(void)
{
    return in_call ? call_state : reg_state;
}

void oma_sip_note_call_state(bool active, oma_state state)
{
    in_call = active;
    call_state = state;
    atomic_store(&busy_with_a_call, active);
}

int oma_sip_account_id(void)
{
    return account_id == PJSUA_INVALID_ID ? -1 : (int)account_id;
}

bool oma_sip_is_registered(void)
{
    return registered;
}

void oma_sip_account_json(cJSON *target)
{
    cJSON_AddStringToObject(target, "uri", account_uri);
    cJSON_AddNumberToObject(target, "status_code", reg_status_code);
    cJSON_AddStringToObject(target, "status_text", reg_status_text);
}

void oma_sip_note_reg_state(bool is_registered, int status_code, const char *status_text)
{
    registered = is_registered;
    reg_status_code = status_code;
    oma_strlcpy(reg_status_text, status_text != NULL ? status_text : "",
                sizeof(reg_status_text));

    if (is_registered)
        reg_state = OMA_STATE_REGISTERED;
    else if (status_code >= 300)
        reg_state = OMA_STATE_REG_FAILED;
    else
        reg_state = OMA_STATE_IDLE;
}

/* ---------------------------------------------------------- pjsua callbacks
 *
 * These run on pjsip's own threads. They copy what they need into an event and
 * return; nothing else in the daemon may be touched from here. */

static void on_reg_state2(pjsua_acc_id acc_id, pjsua_reg_info *info)
{
    struct pjsip_regc_cbparam *param = info->cbparam;
    oma_event ev;

    if (acc_id != account_id)
        return;

    memset(&ev, 0, sizeof(ev));
    ev.type = OMA_EV_REG_STATE;
    ev.status_code = param->code;
    ev.registered = param->code / 100 == 2 && param->expiration > 0;
    ev.expires = (int)param->expiration;
    ev.ts = oma_now();
    if (param->reason.slen > 0) {
        size_t len = (size_t)param->reason.slen;
        if (len >= sizeof(ev.status_text))
            len = sizeof(ev.status_text) - 1;
        memcpy(ev.status_text, param->reason.ptr, len);
        ev.status_text[len] = '\0';
    }
    oma_events_post(&ev);
}


/* Map a final SIP status onto the handful of reasons the history shows. */
static const char *reason_for(int status_code, bool incoming, bool was_connected)
{
    if (was_connected)
        return "completed";
    switch (status_code) {
    case 200:
        return "completed";
    case 486:
    case 600:
        return "busy";
    case 603:
        return "rejected";
    case 487:
        /* We cancelled an outgoing call; for an incoming one the caller gave
         * up before we answered, which is a missed call. */
        return incoming ? "missed" : "cancelled";
    case 408:
    case 480:
        return incoming ? "missed" : "failed";
    default:
        return status_code >= 400 ? "failed" : "completed";
    }
}

static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    pjsua_call_info info;
    oma_event ev;

    (void)e;
    if (pjsua_call_get_info(call_id, &info) != PJ_SUCCESS)
        return;

    memset(&ev, 0, sizeof(ev));
    ev.call_id = (int)call_id;
    ev.ts = oma_now();
    ev.status_code = (int)info.last_status;

    if (info.state == PJSIP_INV_STATE_DISCONNECTED) {
        bool incoming = info.role == PJSIP_ROLE_UAS;

        ev.type = OMA_EV_CALL_ENDED;
        oma_strlcpy(ev.direction, incoming ? "in" : "out", sizeof(ev.direction));
        oma_strlcpy(ev.reason,
                    reason_for((int)info.last_status, incoming,
                               info.connect_duration.sec > 0),
                    sizeof(ev.reason));
        ev.duration_sec = (int)info.connect_duration.sec;
        if (info.last_status_text.slen > 0) {
            size_t len = (size_t)info.last_status_text.slen;

            if (len >= sizeof(ev.status_text))
                len = sizeof(ev.status_text) - 1;
            memcpy(ev.status_text, info.last_status_text.ptr, len);
            ev.status_text[len] = '\0';
        }
        oma_events_post(&ev);
        return;
    }

    ev.type = OMA_EV_CALL_STATE;
    switch (info.state) {
    case PJSIP_INV_STATE_CALLING:
        oma_strlcpy(ev.state, "calling", sizeof(ev.state));
        break;
    case PJSIP_INV_STATE_EARLY:
        oma_strlcpy(ev.state, "early", sizeof(ev.state));
        break;
    case PJSIP_INV_STATE_CONFIRMED:
        oma_strlcpy(ev.state, "connected", sizeof(ev.state));
        break;
    case PJSIP_INV_STATE_DISCONNECTED:
    case PJSIP_INV_STATE_NULL:
    case PJSIP_INV_STATE_INCOMING:
    case PJSIP_INV_STATE_CONNECTING:
    default:
        /* Nothing the panel draws differently; skip the wire traffic. */
        return;
    }
    oma_events_post(&ev);
}

static void on_call_media_state(pjsua_call_id call_id)
{
    pjsua_call_info info;
    oma_event ev;

    if (pjsua_call_get_info(call_id, &info) != PJ_SUCCESS)
        return;

    memset(&ev, 0, sizeof(ev));
    ev.type = OMA_EV_MEDIA_STATE;
    ev.call_id = (int)call_id;
    ev.ts = oma_now();
    ev.active = info.media_status == PJSUA_CALL_MEDIA_ACTIVE;

    if (ev.active && info.conf_slot != PJSUA_INVALID_ID) {
        pjsua_stream_info stream;

        /* Both directions: they hear us, we hear them. */
        pjsua_conf_connect(info.conf_slot, 0);
        pjsua_conf_connect(0, info.conf_slot);

        if (pjsua_call_get_stream_info(call_id, 0, &stream) == PJ_SUCCESS
                && stream.type == PJMEDIA_TYPE_AUDIO) {
            size_t len = (size_t)stream.info.aud.fmt.encoding_name.slen;

            if (len >= sizeof(ev.codec))
                len = sizeof(ev.codec) - 1;
            memcpy(ev.codec, stream.info.aud.fmt.encoding_name.ptr, len);
            ev.codec[len] = '\0';
        }
    }
    oma_events_post(&ev);
}

static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
                             pjsip_rx_data *rdata)
{
    pjsua_call_info info;
    oma_event ev;

    (void)acc_id;
    (void)rdata;

    if (pjsua_call_get_info(call_id, &info) != PJ_SUCCESS)
        return;

    /* One call at a time. Answering 486 here rather than queueing the event
     * keeps the far end from waiting on a decision we have already made. */
    if (atomic_load(&busy_with_a_call)) {
        pjsua_call_answer(call_id, 486, NULL, NULL);
        oma_info("call %d: rejected with 486, another call is in progress",
                 (int)call_id);
        return;
    }

    memset(&ev, 0, sizeof(ev));
    ev.type = OMA_EV_CALL_INCOMING;
    ev.call_id = (int)call_id;
    ev.ts = oma_now();
    oma_strlcpy(ev.direction, "in", sizeof(ev.direction));
    if (info.remote_info.slen > 0) {
        size_t len = (size_t)info.remote_info.slen;

        if (len >= sizeof(ev.remote))
            len = sizeof(ev.remote) - 1;
        memcpy(ev.remote, info.remote_info.ptr, len);
        ev.remote[len] = '\0';
    }
    oma_events_post(&ev);
}

/* ------------------------------------------------------------------- setup */

static pj_str_t cstr(const char *s)
{
    return pj_str((char *)s);
}

static bool using_tls(const oma_config *cfg)
{
    return strcmp(cfg->account.transport, "tls") == 0;
}

/* Give every codec in the config a priority, highest first, and silence every
 * codec that is not listed. That is what makes the `codecs` array an actual
 * preference order rather than a hint. */
static void apply_codec_priorities(const oma_config *cfg)
{
    pjsua_codec_info codecs[64];
    unsigned count = (unsigned)(sizeof(codecs) / sizeof(codecs[0]));
    unsigned i;
    size_t want;

    if (pjsua_enum_codecs(codecs, &count) != PJ_SUCCESS) {
        oma_warn("sip: cannot enumerate codecs");
        return;
    }

    for (i = 0; i < count; i++) {
        pj_uint8_t priority = 0;
        char name[64];
        size_t len = (size_t)codecs[i].codec_id.slen;

        if (len >= sizeof(name))
            len = sizeof(name) - 1;
        memcpy(name, codecs[i].codec_id.ptr, len);
        name[len] = '\0';

        for (want = 0; want < cfg->codec_count; want++) {
            /* pjsua reports "G729/8000/1"; the config says "G729/8000". A
             * prefix match keeps the config free of the channel count. */
            size_t wanted_len = strlen(cfg->codecs[want]);

            if (strncasecmp(name, cfg->codecs[want], wanted_len) == 0
                    && (name[wanted_len] == '\0' || name[wanted_len] == '/')) {
                priority = (pj_uint8_t)(PJMEDIA_CODEC_PRIO_NORMAL
                                        + cfg->codec_count - want);
                break;
            }
        }

        pjsua_codec_set_priority(&codecs[i].codec_id, priority);
        if (priority > 0)
            oma_info("sip: codec %s enabled with priority %u", name, priority);
        else
            oma_dbg("sip: codec %s disabled", name);
    }
}

/* 10/8, 172.16/12, 192.168/16 — the addresses a provider cannot route back to. */
static bool is_private_address(const char *host)
{
    unsigned a = 0, b = 0;

    if (sscanf(host, "%u.%u", &a, &b) != 2)
        return false;
    return a == 10
        || (a == 172 && b >= 16 && b <= 31)
        || (a == 192 && b == 168);
}

/* Behind NAT without STUN, the SDP advertises a LAN address and the far end
 * dutifully sends the audio there. The call connects, the other side hears us,
 * and we hear silence — which looks like a broken microphone on their end, not
 * like a network setting on ours. Say so plainly instead. */
static void warn_if_nat_without_stun(const oma_config *cfg)
{
    pjsua_transport_id ids[8];
    unsigned count = PJ_ARRAY_SIZE(ids);
    unsigned i;

    if (cfg->nat.stun_server[0] != '\0')
        return;
    if (pjsua_enum_transports(ids, &count) != PJ_SUCCESS)
        return;

    for (i = 0; i < count; i++) {
        pjsua_transport_info info;
        char host[64];
        size_t len;

        if (pjsua_transport_get_info(ids[i], &info) != PJ_SUCCESS)
            continue;

        len = (size_t)info.local_name.host.slen;
        if (len == 0 || len >= sizeof(host))
            continue;
        memcpy(host, info.local_name.host.ptr, len);
        host[len] = '\0';

        if (is_private_address(host)) {
            oma_warn("sip: this machine has the private address %s and no STUN "
                     "server is configured. If the registrar returns rport, the "
                     "SDP is corrected from it and calls are fine. If it does "
                     "not, the far end's audio is sent to %s and never arrives: "
                     "the call connects and only one side is heard. Set "
                     "nat.stun_server in config.json if that happens.",
                     host, host);
            return;
        }
    }
}

/* Open the transport the active account needs, if it is not open already. */
static int ensure_transport(const oma_config *cfg, char *err, size_t errlen)
{
    pjsua_transport_config transport;
    bool tls = using_tls(cfg);
    pj_status_t status;

    if ((tls && tls_transport_open) || (!tls && udp_transport_open))
        return 0;

    pjsua_transport_config_default(&transport);
    transport.port = (unsigned)cfg->account.port;

    if (!tls) {
        status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &transport, NULL);
        if (status != PJ_SUCCESS) {
            snprintf(err, errlen, "cannot open the UDP transport on port %d",
                     cfg->account.port);
            return -1;
        }
        udp_transport_open = true;
        oma_info("sip: UDP transport on port %u", transport.port);
        return 0;
    }

    /* 5061 is the registered port for SIP over TLS; a config that still says
     * 5060 almost certainly means "the default", not "5060 for TLS too". */
    if (transport.port == 5060)
        transport.port = 5061;

    transport.tls_setting.method = PJSIP_TLSV1_2_METHOD;
    transport.tls_setting.verify_server = cfg->tls.verify_server ? PJ_TRUE : PJ_FALSE;
    if (cfg->tls.ca_file[0] != '\0')
        transport.tls_setting.ca_list_file = cstr(cfg->tls.ca_file);

    if (!cfg->tls.verify_server) {
        oma_warn("sip: TLS certificate verification is OFF. The signalling is "
                 "encrypted, but anyone able to sit between this machine and "
                 "the PBX can pretend to be it.");
    }

    status = pjsua_transport_create(PJSIP_TRANSPORT_TLS, &transport, NULL);
    if (status != PJ_SUCCESS) {
        char reason[PJ_ERR_MSG_SIZE];

        pj_strerror(status, reason, sizeof(reason));
        snprintf(err, errlen, "cannot open the TLS transport on port %u: %s",
                 transport.port, reason);
        return -1;
    }
    tls_transport_open = true;
    oma_info("sip: TLS transport on port %u, certificate verification %s",
             transport.port, cfg->tls.verify_server ? "on" : "off");
    return 0;
}

static int add_account(const oma_config *cfg, char *err, size_t errlen)
{
    pjsua_acc_config acc;
    char id_uri[512];
    char reg_uri[320];
    const char *suffix;
    pj_status_t status;

    if (!oma_config_has_account(cfg)) {
        oma_info("sip: no account configured, staying unregistered");
        account_uri[0] = '\0';
        reg_state = OMA_STATE_IDLE;
        return 0;
    }

    pjsua_acc_config_default(&acc);

    /* Without ;transport=tls in the URIs, pjsip resolves the destination the
     * usual way and sends over UDP even with a TLS transport open. */
    suffix = using_tls(cfg) ? ";transport=tls" : "";

    if (cfg->account.display_name[0] != '\0')
        snprintf(id_uri, sizeof(id_uri), "\"%s\" <sip:%s@%s%s>",
                 cfg->account.display_name, cfg->account.username,
                 cfg->account.domain, suffix);
    else
        snprintf(id_uri, sizeof(id_uri), "sip:%s@%s%s",
                 cfg->account.username, cfg->account.domain, suffix);
    snprintf(reg_uri, sizeof(reg_uri), "sip:%s%s", cfg->account.domain, suffix);
    snprintf(account_uri, sizeof(account_uri), "sip:%s@%s",
             cfg->account.username, cfg->account.domain);

    acc.id = cstr(id_uri);
    acc.reg_uri = cstr(reg_uri);
    acc.reg_timeout = (unsigned)cfg->account.register_expires;
    acc.ka_interval = (unsigned)cfg->nat.keepalive_sec;

    /* Behind NAT the Contact we advertise is a LAN address; letting pjsip
     * rewrite it from the registrar's response is what makes inbound calls
     * arrive at all on a home connection. */
    acc.allow_contact_rewrite = PJ_TRUE;
    acc.contact_rewrite_method = 2;

    /* The registrar tells us our public address in the Via `received` field of
     * every REGISTER response. pjsua knows it but, by default, still writes the
     * LAN address into the SDP, so the far end sends the audio somewhere it
     * cannot arrive. Turning this on uses the address the registrar saw, which
     * fixes one-way audio behind NAT without a STUN server, an extra round trip
     * at startup, or telling a third party every time the phone comes up.
     * pjsua ignores it when STUN or ICE is configured, so it costs nothing to
     * leave on for the people who do set a STUN server. */
    acc.allow_sdp_nat_rewrite = PJ_TRUE;
    acc.use_rfc5626 = 0;          /* plain UDP, no outbound */
    acc.register_on_acc_add = PJ_TRUE;

    /* pjsua retries a failed REGISTER after PJSUA_REG_RETRY_INTERVAL, which is
     * 300 seconds. That is a sensible default for a server that registers a
     * thousand trunks; for a desktop phone it means five minutes of not
     * ringing after a Wi-Fi blip. Retry every 20s instead, with a few seconds
     * of jitter so a room full of Omarchy machines coming back from a power
     * cut does not hit the registrar in lockstep. */
    acc.reg_first_retry_interval = 3;
    acc.reg_retry_interval = 20;
    acc.reg_retry_random_interval = 5;

    if (cfg->account.proxy[0] != '\0') {
        acc.proxy[0] = cstr(cfg->account.proxy);
        acc.proxy_cnt = 1;
    }

    acc.cred_count = 1;
    acc.cred_info[0].realm = cstr("*");
    acc.cred_info[0].scheme = cstr("digest");
    acc.cred_info[0].username = cstr(cfg->account.auth_username[0] != '\0'
                                     ? cfg->account.auth_username
                                     : cfg->account.username);
    acc.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    acc.cred_info[0].data = cstr(cfg->account.password);

    status = pjsua_acc_add(&acc, PJ_TRUE, &account_id);
    if (status != PJ_SUCCESS) {
        char reason[PJ_ERR_MSG_SIZE];

        pj_strerror(status, reason, sizeof(reason));
        snprintf(err, errlen, "cannot add the SIP account: %s", reason);
        return -1;
    }

    reg_state = OMA_STATE_REGISTERING;
    oma_info("sip: account %s registering against %s", account_uri, reg_uri);
    return 0;
}

int oma_sip_start(const oma_config *cfg, char *err, size_t errlen)
{
    pjsua_config ua;
    pjsua_logging_config logging;
    pjsua_media_config media;
    char log_path[512];
    char user_agent[64];
    pj_status_t status;

    active_config = *cfg;

    status = pjsua_create();
    if (status != PJ_SUCCESS) {
        snprintf(err, errlen, "pjsua_create failed");
        return -1;
    }
    pjsua_up = true;

    pjsua_config_default(&ua);
    ua.cb.on_reg_state2 = &on_reg_state2;
    ua.cb.on_call_state = &on_call_state;
    ua.cb.on_call_media_state = &on_call_media_state;
    ua.cb.on_incoming_call = &on_incoming_call;
    snprintf(user_agent, sizeof(user_agent), "Omacall/%s", OMACALL_VERSION);
    ua.user_agent = cstr(user_agent);
    if (cfg->nat.stun_server[0] != '\0') {
        ua.stun_srv[0] = cstr(cfg->nat.stun_server);
        ua.stun_srv_cnt = 1;
    }

    pjsua_logging_config_default(&logging);
    logging.level = (unsigned)cfg->log_level;
    logging.console_level = 1;
    logging.msg_logging = PJ_TRUE;
    if (oma_path_state(log_path, sizeof(log_path), "pjsip.log") == 0)
        logging.log_filename = cstr(log_path);

    pjsua_media_config_default(&media);
    /* The conference bridge and the sound device must agree on the rate. They
     * do not have to in general, but the WebRTC echo canceller is created for
     * the bridge's rate and then handed the device's frames: at 16k bridge
     * against a 48k device it aborts on
     * `WebRtcAec_ProcessFrames: Assertion aec->num_bands == num_bands failed`,
     * which takes the whole daemon down the moment a call starts. 16 kHz is
     * the right side to meet on for telephony — every codec in v1 is 8 or 16k,
     * and PipeWire resamples to its own 48k without complaint. */
    media.clock_rate = 16000;
    media.snd_clock_rate = 16000;
    media.channel_count = 1;
    media.ec_tail_len = cfg->audio.echo_cancel ? 200 : 0;
    media.no_vad = PJ_FALSE;
    /* Release the capture device when idle so Omacall does not sit on the
     * microphone between calls. */
    media.snd_auto_close_time = 1;

    status = pjsua_init(&ua, &logging, &media);
    if (status != PJ_SUCCESS) {
        snprintf(err, errlen, "pjsua_init failed");
        return -1;
    }

    if (ensure_transport(cfg, err, errlen) != 0)
        return -1;

    status = pjsua_start();
    if (status != PJ_SUCCESS) {
        snprintf(err, errlen, "pjsua_start failed");
        return -1;
    }

    apply_codec_priorities(cfg);

    /* The tone generator hangs off the conference bridge, so it can only be
     * built once the bridge exists. */
    if (oma_tones_init(16000, cfg->audio.ring_volume) != 0)
        oma_warn("sip: no tone generator, calls will ring silently");

    warn_if_nat_without_stun(cfg);

    return add_account(cfg, err, errlen);
}

void oma_sip_stop(void)
{
    if (!pjsua_up)
        return;

    if (account_id != PJSUA_INVALID_ID) {
        /* Best effort un-REGISTER so the registrar stops sending us calls the
         * moment the shell shuts the daemon down. */
        pjsua_acc_set_registration(account_id, PJ_FALSE);
        pjsua_acc_del(account_id);
        account_id = PJSUA_INVALID_ID;
    }
    oma_tones_shutdown();
    pjsua_destroy();
    udp_transport_open = false;
    tls_transport_open = false;
    pjsua_up = false;
    registered = false;
    reg_state = OMA_STATE_IDLE;
    in_call = false;
}

int oma_sip_set_registration(bool enabled, char *err, size_t errlen)
{
    pj_status_t status;

    if (account_id == PJSUA_INVALID_ID) {
        snprintf(err, errlen, "no SIP account is configured");
        return -1;
    }
    status = pjsua_acc_set_registration(account_id, enabled ? PJ_TRUE : PJ_FALSE);
    if (status != PJ_SUCCESS) {
        snprintf(err, errlen, "cannot change the registration");
        return -1;
    }
    if (enabled)
        reg_state = OMA_STATE_REGISTERING;
    return 0;
}

/* True when anything the registrar cares about changed. */
static bool account_changed(const oma_account *a, const oma_account *b)
{
    return strcmp(a->username, b->username) != 0
        || strcmp(a->auth_username, b->auth_username) != 0
        || strcmp(a->password, b->password) != 0
        || strcmp(a->domain, b->domain) != 0
        || strcmp(a->proxy, b->proxy) != 0
        || strcmp(a->display_name, b->display_name) != 0
        || a->register_expires != b->register_expires;
}

int oma_sip_reconfigure(const oma_config *cfg, char *err, size_t errlen)
{
    bool account_moved = account_changed(&active_config.account, &cfg->account);

    apply_codec_priorities(cfg);
    oma_log_set_level(cfg->log_level >= 4 ? OMA_LOG_DEBUG : OMA_LOG_INFO);

    if (!account_moved) {
        active_config = *cfg;
        return 0;
    }

    /* The account identity changed, so the old registration is meaningless:
     * drop it and add the new one rather than trying to patch it in place. */
    if (account_id != PJSUA_INVALID_ID) {
        pjsua_acc_set_registration(account_id, PJ_FALSE);
        pjsua_acc_del(account_id);
        account_id = PJSUA_INVALID_ID;
        registered = false;
        reg_status_code = 0;
        reg_status_text[0] = '\0';
    }

    active_config = *cfg;

    if (ensure_transport(cfg, err, errlen) != 0)
        return -1;
    return add_account(cfg, err, errlen);
}
