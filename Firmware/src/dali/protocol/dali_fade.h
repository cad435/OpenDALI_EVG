/*
    dali_fade.h - DALI fade engine (IEC 62386-102 §9.5)

    Manages timed arc power transitions (fadeTime for DAPC/scenes,
    fadeRate for UP/DOWN). Called from main loop via dali_fade_tick().
*/
#ifndef _DALI_FADE_H
#define _DALI_FADE_H

#include <stdint.h>

/* Start a fade from current level to target over duration_ms.
 * If duration_ms == 0, sets level instantly. Calls arc_callback.
 * target 0 (OFF) fades down to minLevel and switches off at the end. */
void dali_fade_start(uint8_t target, uint32_t duration_ms);

/* Start a fade from current level to target at ms_per_step rate.
 * Used by UP/DOWN commands (fadeRate-based). */
void dali_fade_start_rate(uint8_t target, uint16_t ms_per_step);

/* Stop any running fade immediately (level stays at current). */
void dali_fade_stop(void);

/* Main loop tick — steps the fade one level when enough time has elapsed. */
void dali_fade_tick(void);

/* Returns 1 if a fade is currently in progress. */
uint8_t dali_fade_is_running(void);

/* Compute effective fade time in ms (standard fade_time, or extended). */
uint32_t dali_fade_get_effective_ms(void);

/* ── DT8 colour crossfade (IEC 62386-209) ───────────────────────────
 * Fade the active colour (ds.colour_actual[]) toward `target` over
 * duration_ms, running in parallel with any brightness fade. Stepped
 * from dali_fade_tick(). If duration_ms == 0 or the colour is already
 * at target, it is applied instantly. Only defined when EVG_HAS_DT8. */
void dali_fade_colour_start(const volatile uint8_t *target, uint32_t duration_ms);

/* Stop a running colour fade immediately (colour stays at current). */
void dali_fade_colour_stop(void);

/* Returns 1 if a colour fade is currently in progress. */
uint8_t dali_fade_colour_is_running(void);

#endif /* _DALI_FADE_H */
