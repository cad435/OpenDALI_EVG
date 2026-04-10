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

    if (target == ds.actual_level || duration_ms == 0 || target == 0) {
        ds.actual_level = target;
        if (ds.arc_callback) ds.arc_callback(target);
        return;
    }

    target_level = target;
    uint16_t steps = (ds.actual_level > target)
                   ? (ds.actual_level - target)
                   : (target - ds.actual_level);
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

void dali_fade_tick(void) {
    if (!fade_running) return;

    uint32_t now = millis();
    if (now - last_step_ms < fade_ms_per_step) return;
    last_step_ms = now;

    if (ds.actual_level < target_level) {
        ds.actual_level++;
    } else if (ds.actual_level > target_level) {
        ds.actual_level--;
    }

    if (ds.arc_callback) ds.arc_callback(ds.actual_level);

    if (ds.actual_level == target_level) {
        fade_running = 0;
    }
}

uint8_t dali_fade_is_running(void) {
    return fade_running;
}
