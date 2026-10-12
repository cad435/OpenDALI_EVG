/*
    dali_fade.c - DALI fade engine (IEC 62386-102 §9.5)

    Handles timed transitions between arc power levels. Supports:
    - fadeTime: total transition duration (DAPC, scenes)
    - fadeRate: ms per step (UP/DOWN commands)
    - Extended fade time (DALI-2): base × multiplier

    The fade engine reads/writes ds.actual_level and calls ds.arc_callback
    from the main loop context (dali_fade_tick).
*/

#include "dali_fade.h"
#include "../dali_state.h"
#include "../dali_physical.h"

/* millis() provided by main.c */
extern uint32_t millis(void);

/* ── Fade engine state (module-private) ──────────────────────────── */
static volatile uint8_t     fade_running = 0;
static volatile uint8_t     target_level = 0;
static volatile uint16_t    fade_ms_per_step = 0;
static volatile uint32_t    last_step_ms = 0;

#if EVG_HAS_DT8
/* ── Colour crossfade state (module-private) ─────────────────────────
 * Independent of the brightness fade above; both can run concurrently.
 * The colour fade linearly interpolates each RGBW channel from its value
 * at fade start toward the target. The number of steps equals the largest
 * per-channel distance, so the fastest-moving channel advances one unit
 * per step and every channel arrives on its target at the final step. */
static volatile uint8_t     colour_fade_running = 0;
static volatile uint8_t     colour_start[4];
static volatile uint8_t     colour_target[4];
static volatile uint16_t    colour_total_steps = 0;
static volatile uint16_t    colour_step = 0;
static volatile uint16_t    colour_ms_per_step = 0;
static volatile uint32_t    colour_last_step_ms = 0;
#endif

/* ================================================================== *
 *  get_ext_fade_time_ms() — DALI-2 extended fade time                 *
 * ================================================================== */
static uint32_t get_ext_fade_time_ms(void) {
    static const uint16_t mult_ms[5] = { 0, 100, 1000, 10000, 60000 };
    if (ds.ext_fade_mult == 0 || ds.ext_fade_mult > 4 || ds.ext_fade_base == 0) return 0;
    return (uint32_t)ds.ext_fade_base * mult_ms[ds.ext_fade_mult];
}

/* ================================================================== *
 *  PUBLIC API                                                         *
 * ================================================================== */

uint32_t dali_fade_get_effective_ms(void) {
    if (ds.fade_time > 0)
        return dali_fade_time_ms[ds.fade_time];
    return get_ext_fade_time_ms();
}

void dali_fade_start(uint8_t target, uint32_t duration_ms) {
    fade_running = 0;

    if (target == ds.actual_level || duration_ms == 0) {
        ds.actual_level = target;
        if (ds.arc_callback) ds.arc_callback(target);
        return;
    }

    /* Fading to OFF: dim down to minLevel, then switch off as the last
     * step at the end of the fade time (IEC 62386-102 §9.5). */
    uint8_t to = target ? target : ds.min_level;

    target_level = target;
    uint16_t steps = (ds.actual_level > to)
                   ? (ds.actual_level - to)
                   : (to - ds.actual_level);
    if (target == 0) steps++;   /* final minLevel → OFF step */
    fade_ms_per_step = duration_ms / steps;
    if (fade_ms_per_step < 1) fade_ms_per_step = 1;
    last_step_ms = millis();
    fade_running = 1;
}

void dali_fade_start_rate(uint8_t target, uint16_t ms_per_step) {
    fade_running = 0;

    if (target == ds.actual_level) return;

    target_level = target;
    fade_ms_per_step = ms_per_step;
    if (fade_ms_per_step < 1) fade_ms_per_step = 1;
    last_step_ms = millis();
    fade_running = 1;
}

void dali_fade_stop(void) {
    fade_running = 0;
}

#if EVG_HAS_DT8
/* ================================================================== *
 *  COLOUR CROSSFADE (DT8) — runs in parallel with the brightness fade *
 * ================================================================== */

void dali_fade_colour_start(const volatile uint8_t *target, uint32_t duration_ms) {
    colour_fade_running = 0;

    uint16_t max_delta = 0;
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t s = ds.colour_actual[i];
        uint8_t t = target[i];
        uint16_t d = (s > t) ? (uint16_t)(s - t) : (uint16_t)(t - s);
        if (d > max_delta) max_delta = d;
        colour_start[i]  = s;
        colour_target[i] = t;
    }

    /* No fade time, or nothing to change → apply instantly. */
    if (duration_ms == 0 || max_delta == 0) {
        for (uint8_t i = 0; i < 4; i++)
            ds.colour_actual[i] = target[i];
        if (ds.colour_callback)
            ds.colour_callback((const uint8_t *)ds.colour_actual, EVG_NUM_COLOURS);
        return;
    }

    colour_total_steps  = max_delta;
    colour_step         = 0;
    colour_ms_per_step  = (uint16_t)(duration_ms / max_delta);
    if (colour_ms_per_step < 1) colour_ms_per_step = 1;
    colour_last_step_ms = millis();
    colour_fade_running = 1;
}

void dali_fade_colour_stop(void) {
    colour_fade_running = 0;
}

uint8_t dali_fade_colour_is_running(void) {
    return colour_fade_running;
}

static void colour_fade_tick(void) {
    if (!colour_fade_running) return;

    uint32_t now = millis();
    if (now - colour_last_step_ms < colour_ms_per_step) return;
    colour_last_step_ms = now;

    colour_step++;
    if (colour_step >= colour_total_steps) {
        /* Final step: land exactly on the target (no rounding drift). */
        for (uint8_t i = 0; i < 4; i++)
            ds.colour_actual[i] = colour_target[i];
        colour_fade_running = 0;
    } else {
        for (uint8_t i = 0; i < 4; i++) {
            int32_t span = (int32_t)colour_target[i] - (int32_t)colour_start[i];
            ds.colour_actual[i] = (uint8_t)((int32_t)colour_start[i]
                                  + (span * (int32_t)colour_step) / (int32_t)colour_total_steps);
        }
    }

    if (ds.colour_callback)
        ds.colour_callback((const uint8_t *)ds.colour_actual, EVG_NUM_COLOURS);
}
#endif /* EVG_HAS_DT8 */

void dali_fade_tick(void) {
    if (fade_running) {
        uint32_t now = millis();
        if (now - last_step_ms >= fade_ms_per_step) {
            last_step_ms = now;

            if (ds.actual_level < target_level) {
                ds.actual_level++;
            } else if (ds.actual_level > target_level) {
                /* Fade to OFF: below minLevel there is no arc power,
                 * so the step after minLevel switches off. */
                if (target_level == 0 && ds.actual_level <= ds.min_level)
                    ds.actual_level = 0;
                else
                    ds.actual_level--;
            }

            if (ds.arc_callback) ds.arc_callback(ds.actual_level);

            if (ds.actual_level == target_level) {
                fade_running = 0;
            }
        }
    }

#if EVG_HAS_DT8
    colour_fade_tick();   /* advance colour crossfade (independent of brightness) */
#endif
}

uint8_t dali_fade_is_running(void) {
    return fade_running;
}
