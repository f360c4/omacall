#include "tones.h"

#include "log.h"

#include <pjsua-lib/pjsua.h>

#define FRAME_PTIME_MS 20

static pj_pool_t *pool;
static pjmedia_port *tonegen;
static pjsua_conf_port_id slot = PJSUA_INVALID_ID;
static bool playing;
static short tone_volume;
static int dtmf_slot = PJSUA_INVALID_ID;   /* call slot we are sending DTMF to */

int oma_tones_init(unsigned clock_rate, double volume)
{
    unsigned samples_per_frame = clock_rate * FRAME_PTIME_MS / 1000;
    pj_status_t status;

    pool = pjsua_pool_create("omacall-tones", 512, 512);
    if (pool == NULL) {
        oma_err("tones: cannot create the memory pool");
        return -1;
    }

    /* Clamp to something audible but not startling; 0 would mean "use the
     * generator's own default", which ignores the user's setting. */
    if (volume < 0.05)
        volume = 0.05;
    if (volume > 1.0)
        volume = 1.0;
    tone_volume = (short)(volume * 16383);

    /* No LOOP here: it is a property of each playback, and inband DTMF must
     * play exactly once. The generator comes with the DTMF digit map loaded. */
    status = pjmedia_tonegen_create(pool, clock_rate, 1, samples_per_frame, 16,
                                    0, &tonegen);
    if (status != PJ_SUCCESS) {
        oma_err("tones: cannot create the tone generator");
        return -1;
    }

    status = pjsua_conf_add_port(pool, tonegen, &slot);
    if (status != PJ_SUCCESS) {
        oma_err("tones: cannot attach the tone generator to the bridge");
        slot = PJSUA_INVALID_ID;
        return -1;
    }

    oma_dbg("tones: ready at %u Hz on conference slot %d", clock_rate, slot);
    return 0;
}

void oma_tones_shutdown(void)
{
    oma_tones_stop();
    if (slot != PJSUA_INVALID_ID) {
        pjsua_conf_remove_port(slot);
        slot = PJSUA_INVALID_ID;
    }
    tonegen = NULL;
    if (pool != NULL) {
        pj_pool_release(pool);
        pool = NULL;
    }
}

static void play(short on_msec, short off_msec)
{
    pjmedia_tone_desc tone;

    if (tonegen == NULL || slot == PJSUA_INVALID_ID)
        return;

    oma_tones_stop();

    pj_bzero(&tone, sizeof(tone));
    tone.freq1 = 425;
    tone.freq2 = 0;
    tone.on_msec = on_msec;
    tone.off_msec = off_msec;
    tone.volume = tone_volume;

    if (pjmedia_tonegen_play(tonegen, 1, &tone, PJMEDIA_TONEGEN_LOOP) != PJ_SUCCESS) {
        oma_warn("tones: cannot start the tone");
        return;
    }
    /* Slot 0 is the sound device. The tone goes to the speaker only — never to
     * the call, or the far end would hear our own ringback. */
    if (pjsua_conf_connect(slot, 0) != PJ_SUCCESS) {
        oma_warn("tones: cannot route the tone to the speaker");
        pjmedia_tonegen_stop(tonegen);
        return;
    }
    playing = true;
}

void oma_tones_ringback(void)
{
    play(1000, 4000);
}

void oma_tones_ring(void)
{
    play(1000, 4000);
}

void oma_tones_stop(void)
{
    if (!playing)
        return;
    if (slot != PJSUA_INVALID_ID)
        pjsua_conf_disconnect(slot, 0);
    if (tonegen != NULL) {
        pjmedia_tonegen_stop(tonegen);
        pjmedia_tonegen_rewind(tonegen);
    }
    playing = false;
}

bool oma_tones_playing(void)
{
    return playing;
}

int oma_tones_dtmf(const char *digits, int call_slot)
{
    pjmedia_tone_digit tones[32];
    unsigned count = 0;
    bool into_call = call_slot != OMA_TONES_LOCAL_ONLY
                     && call_slot != PJSUA_INVALID_ID;

    if (tonegen == NULL || slot == PJSUA_INVALID_ID)
        return -1;

    for (; digits[count] != '\0' && count < PJ_ARRAY_SIZE(tones); count++) {
        tones[count].digit = digits[count];
        tones[count].on_msec = 120;
        tones[count].off_msec = 70;
        tones[count].volume = tone_volume;
    }
    if (count == 0)
        return -1;

    oma_tones_stop();
    if (pjmedia_tonegen_play_digits(tonegen, count, tones, 0) != PJ_SUCCESS) {
        oma_warn("tones: cannot play inband DTMF");
        return -1;
    }

    /* The speaker always. The call only for inband, where the tone is the
     * signal rather than the feedback. */
    if (pjsua_conf_connect(slot, 0) != PJ_SUCCESS) {
        pjmedia_tonegen_stop(tonegen);
        return -1;
    }
    if (into_call && pjsua_conf_connect(slot, (pjsua_conf_port_id)call_slot)
            != PJ_SUCCESS) {
        pjsua_conf_disconnect(slot, 0);
        pjmedia_tonegen_stop(tonegen);
        return -1;
    }

    dtmf_slot = into_call ? call_slot : OMA_TONES_LOCAL_ONLY;
    return 0;
}

void oma_tones_dtmf_poll(void)
{
    if (dtmf_slot == PJSUA_INVALID_ID)
        return;
    if (tonegen != NULL && pjmedia_tonegen_is_busy(tonegen))
        return;

    if (dtmf_slot != OMA_TONES_LOCAL_ONLY)
        pjsua_conf_disconnect(slot, (pjsua_conf_port_id)dtmf_slot);
    pjsua_conf_disconnect(slot, 0);
    if (tonegen != NULL)
        pjmedia_tonegen_rewind(tonegen);
    dtmf_slot = PJSUA_INVALID_ID;
}
