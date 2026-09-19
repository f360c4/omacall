#ifndef OMA_CALLS_H
#define OMA_CALLS_H

#include "config.h"
#include "events.h"
#include "sip.h"

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>

/* One call at a time, by design. A second INVITE arriving during a call is
 * answered 486 Busy Here and written to the history as a missed call — that is
 * the whole of Omacall's call waiting story, and it is deliberate. */

typedef struct {
    int     id;                /* pjsua call id, or -1 when idle */
    char    direction[4];      /* "in" or "out" */
    char    remote[256];
    char    number[128];
    char    display[128];
    char    codec[32];
    int64_t created_at;        /* when the call appeared */
    int64_t connected_at;      /* when it was answered, or 0 */
    bool    muted;
    bool    media_active;
    oma_state state;
} oma_call;

void oma_calls_init(const oma_config *cfg);
void oma_calls_shutdown(void);

/* `dest` may be a bare number or a full sip: URI. A bare number is completed
 * with the account's domain. */
int  oma_calls_place(const char *dest, int *call_id, char *err, size_t errlen);
int  oma_calls_answer(int call_id, char *err, size_t errlen);
int  oma_calls_hangup(int call_id, char *err, size_t errlen);
int  oma_calls_dtmf(const char *digits, char *err, size_t errlen);

/* Play a key tone on this machine's speaker and nowhere else. This is the
 * click a phone gives you while you are still dialling, before there is any
 * call to send a digit to. */
int  oma_calls_keytone(const char *digits, char *err, size_t errlen);
int  oma_calls_set_muted(bool muted, char *err, size_t errlen);

bool oma_calls_active(void);
const oma_call *oma_calls_current(void);
/* The `call` field of status and hello: an object, or JSON null. */
cJSON *oma_calls_json(void);

/* Main loop only: turn a queued pjsip event into state, history and broadcasts. */
void oma_calls_handle_event(const oma_event *ev);
/* Main loop only: fire the ring timeout. Cheap, called once per poll cycle. */
void oma_calls_tick(void);

#endif /* OMA_CALLS_H */
