#ifndef OMA_EVENTS_H
#define OMA_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

/* The queue between pjsip's worker threads and the daemon's main loop.
 *
 * pjsua calls its callbacks from threads we do not own. Serializing JSON,
 * writing to client sockets or appending to the history from inside one of
 * those is how a softphone gets a data race. So a callback fills one of these
 * flat structs, drops it in the queue and writes a byte to a pipe; poll() in
 * the main loop wakes up and everything else happens there.
 *
 * The struct is deliberately flat and fixed-size: no allocation happens on the
 * pjsip side, so posting an event cannot fail for lack of memory. */

typedef enum {
    OMA_EV_REG_STATE,
    OMA_EV_CALL_INCOMING,
    OMA_EV_CALL_STATE,
    OMA_EV_CALL_ENDED,
    OMA_EV_MEDIA_STATE,
    OMA_EV_ERROR
} oma_event_type;

typedef struct {
    oma_event_type type;

    int     call_id;
    int     status_code;
    int     expires;
    int     duration_sec;
    int64_t ts;
    bool    registered;
    bool    active;

    char state[16];          /* calling, early, connected, disconnecting */
    char direction[4];       /* in, out */
    char remote[256];        /* full SIP URI */
    char number[128];        /* user part */
    char display[128];
    char reason[16];
    char codec[32];
    char status_text[128];
    char code[32];           /* error code, for OMA_EV_ERROR */
    char message[256];
} oma_event;

int  oma_events_init(void);
void oma_events_shutdown(void);

/* Read end of the wakeup pipe, for poll(). */
int  oma_events_fd(void);

/* Safe to call from any thread. A full queue drops the event with a warning
 * rather than blocking a media thread. */
void oma_events_post(const oma_event *ev);

/* Main loop only. Returns 1 when `ev` was filled, 0 when the queue is empty. */
int  oma_events_next(oma_event *ev);

/* Drain the pipe's wakeup bytes. Call once poll() reports the fd readable. */
void oma_events_drain_pipe(void);

#endif /* OMA_EVENTS_H */
