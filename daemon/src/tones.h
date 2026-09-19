#ifndef OMA_TONES_H
#define OMA_TONES_H

#include <stdbool.h>

/* Ringback and ring, synthesized rather than played from a file.
 *
 * A tone generator hanging off the conference bridge costs nothing to ship, is
 * never missing, and cannot be the wrong sample rate. Both patterns follow the
 * Brazilian convention (425 Hz, one second on, four off), which is also what
 * most of Europe uses.
 *
 * Only one tone plays at a time, so a single generator and slot is enough. */

int  oma_tones_init(unsigned clock_rate, double volume);
void oma_tones_shutdown(void);

/* Local ringback while an outgoing call is ringing, for when the far end
 * sends no early media of its own. */
void oma_tones_ringback(void);

/* Ring for an incoming call. */
void oma_tones_ring(void);

void oma_tones_stop(void);
bool oma_tones_playing(void);

/* Play DTMF digits as audible tones.
 *
 * With a real `call_slot` the tones go into the call as well as to our own
 * speaker: that is inband DTMF, for gateways that understand nothing else.
 * With OMA_TONES_LOCAL_ONLY they go to the speaker alone — the digit itself
 * travelled out of band, and this is only so the person pressing a key hears
 * something, the way every desk phone does.
 *
 * Returns 0 when the digits were queued. */
#define OMA_TONES_LOCAL_ONLY (-1)
int  oma_tones_dtmf(const char *digits, int call_slot);

/* Disconnect the generator from the call once the digits have finished.
 * Called from the main loop's tick; doing it any earlier would clip them. */
void oma_tones_dtmf_poll(void);

#endif /* OMA_TONES_H */
