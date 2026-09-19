#ifndef OMA_SIP_H
#define OMA_SIP_H

#include "config.h"

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>

/* The pjsua side of the daemon: one account, UDP, one call at a time.
 *
 * Everything here runs on the main loop. pjsua's own threads only ever reach
 * the rest of the daemon through the event queue in events.h. */

typedef enum {
    OMA_STATE_IDLE,
    OMA_STATE_REGISTERING,
    OMA_STATE_REGISTERED,
    OMA_STATE_REG_FAILED,
    OMA_STATE_INCOMING,
    OMA_STATE_CALLING,
    OMA_STATE_EARLY,
    OMA_STATE_CONNECTED,
    OMA_STATE_DISCONNECTING
} oma_state;

const char *oma_state_name(oma_state state);

/* Bring pjsua up: transport, codecs, account. Without a configured account it
 * still starts, just unregistered, so the panel has something to talk to. */
int  oma_sip_start(const oma_config *cfg, char *err, size_t errlen);

/* Un-REGISTER, hang up, tear pjsua down. */
void oma_sip_stop(void);

/* Apply a changed config: codec priorities always, the account only when one
 * of its fields actually moved. */
int  oma_sip_reconfigure(const oma_config *cfg, char *err, size_t errlen);

/* Force a REGISTER or un-REGISTER without touching the config. */
int  oma_sip_set_registration(bool enabled, char *err, size_t errlen);

oma_state oma_sip_state(void);
bool oma_sip_is_registered(void);

/* The account's registration status, for `status` and `hello`. */
void oma_sip_account_json(cJSON *target);

/* Called from the main loop when an event says the registration moved. */
void oma_sip_note_reg_state(bool registered, int status_code, const char *status_text);

/* Called from the main loop as a call moves. While a call exists its state is
 * what oma_sip_state() reports; the registration state returns underneath. */
void oma_sip_note_call_state(bool in_call, oma_state state);

/* The pjsua account id, or -1. calls.c needs it to place a call. */
int oma_sip_account_id(void);

#endif /* OMA_SIP_H */
