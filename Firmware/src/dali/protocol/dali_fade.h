/*
    dali_fade.h - DALI fade engine (IEC 62386-102 §9.5)

    Manages timed arc power transitions (fadeTime for DAPC/scenes,
    fadeRate for UP/DOWN). Called from main loop via dali_fade_tick().
*/
#ifndef _DALI_FADE_H
#define _DALI_FADE_H

#include <stdint.h>

/* Start a fade from current level to target over duration_ms.
 * If duration_ms == 0, sets level instantly. Calls arc_callback. */
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

#endif /* _DALI_FADE_H */
