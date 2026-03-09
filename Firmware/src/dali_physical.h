/*
    dali_physical.h - DALI physical layer timing constants (IEC 62386-101)
                    + protocol command numbers (IEC 62386-102)

    DALI uses Manchester encoding at 1200 baud:
    - Te (half-bit period) = 1/2400 s = 416.67 µs
    - Each data bit = 2 half-bits = 2 Te = 833.33 µs
    - Forward frame: 1 start + 16 data + 2 stop = 19 bits = 38 Te ≈ 15.8 ms
    - Backward frame: 1 start + 8 data + 2 stop = 11 bits = 22 Te ≈ 9.2 ms

    Manchester encoding (NO_PHY, active=LOW):
    ┌────────────────────────────────────────────────┐
    │ Bit 1: active→idle = LOW first half, HIGH 2nd │
    │ Bit 0: idle→active = HIGH first half, LOW 2nd │
    │        ┌───┐       ┌───┐                      │
    │ Bit 1: │   │  Bit 0:   │   │                  │
    │    ────┘   │       │   └────                   │
    │            └───────┘                           │
    └────────────────────────────────────────────────┘

    Timer setup:
    - TIM2 runs at 1 MHz (48 MHz / prescaler 48 = 1 µs/tick)
    - CH2: output compare for TX Te tick (backward frame generation)
    - CH4: output compare for RX idle timeout (frame-complete detection)
*/
#ifndef _DALI_PHYSICAL_H
#define _DALI_PHYSICAL_H

#include <stdint.h>

/* ── Fundamental timing ──────────────────────────────────────────── */
#define DALI_BAUD           1200    /* DALI bit rate (bits/second) */
#define DALI_TE             417     /* Te in µs (416.67 rounded up) */

/* ── TIM2 configuration ──────────────────────────────────────────── */
/* Timer clock = HCLK / (PSC+1) = 48 MHz / 48 = 1 MHz → 1 µs/tick */
#define DALI_TIMER_PSC      47      /* Prescaler value (PSC register) */
#define DALI_TIMER_ARR      65535   /* Free-running 16-bit counter */

/* ── RX timing thresholds (in timer ticks = µs) ─────────────────── *
 * These define the acceptance windows for edge-to-edge intervals.
 * Widened to ±30% of nominal for robust reception via EXTI.
 * IEC 62386-101 requires gear to accept ±10% minimum.
 * ──────────────────────────────────────────────────────────────────*/
#define DALI_TE_TICKS       417     /* Nominal Te in timer ticks */
#define DALI_TE_MIN         292     /* 0.7 × Te — shortest valid Te */
#define DALI_TE_MAX         542     /* 1.3 × Te — longest valid Te */
#define DALI_2TE_MIN        584     /* 0.7 × 2Te — shortest valid 2Te */
#define DALI_2TE_MAX        1084    /* 1.3 × 2Te — longest valid 2Te */

/* ── Frame detection ─────────────────────────────────────────────── *
 * DALI_IDLE_TIMEOUT: If no edge arrives within 5 Te after the last
 * edge, the frame is considered complete. The stop condition requires
 * ≥ 2 Te of idle; 5 Te provides margin for edge jitter.
 * ──────────────────────────────────────────────────────────────────*/
#define DALI_IDLE_TIMEOUT   (5 * DALI_TE_TICKS)    /* 2085 µs */

/* ── Backward frame settle time ──────────────────────────────────── *
 * IEC 62386-101: backward frame must start 7–22 Te AFTER the forward
 * frame ends (including stop bits). The idle timeout fires ~3 Te after
 * frame end, plus main loop latency (~0 Te), plus DALI_SETTLE_TE
 * counted in the TX state machine, plus 1 Te for the start bit to
 * begin. Total ≈ 3 + 0 + 10 + 1 = 14 Te from forward frame end.
 * ──────────────────────────────────────────────────────────────────*/
#define DALI_SETTLE_TE      10      /* Te ticks to wait before TX start */

/* ── Edge classification macros ──────────────────────────────────── *
 * Used by the RX ISR to classify edge-to-edge intervals:
 * - DALI_IS_TE:  interval matches a single half-bit period
 * - DALI_IS_2TE: interval matches a double half-bit period
 *                (occurs when same level spans a bit boundary)
 * ──────────────────────────────────────────────────────────────────*/
#define DALI_IS_TE(dt)      ((DALI_TE_MIN) <= (dt) && (dt) <= (DALI_TE_MAX))
#define DALI_IS_2TE(dt)     ((DALI_2TE_MIN) <= (dt) && (dt) <= (DALI_2TE_MAX))

/* ── TIM2 channel assignments ────────────────────────────────────── */
#define DALI_TX_CHANNEL     2   /* CH2 output compare → TX Te tick ISR */
#define DALI_IDLE_CHANNEL   4   /* CH4 output compare → RX idle timeout ISR */

/* ── DALI indirect commands (IEC 62386-102 §9.5, data byte when S=1) ──
 * Commands 0–31:   immediate action commands
 * Commands 32–128: configuration commands (require config repeat: 2× within 100 ms)
 * Commands 144–255: query commands (require backward frame response)
 * ──────────────────────────────────────────────────────────────────*/

/* Immediate action commands (0–31) */
#define DALI_CMD_OFF                0   /* Immediately set level to 0 */
#define DALI_CMD_UP                 1   /* Fade up at fadeRate (continuous) */
#define DALI_CMD_DOWN               2   /* Fade down at fadeRate (continuous) */
#define DALI_CMD_STEP_UP            3   /* Increase level by 1 (instant) */
#define DALI_CMD_STEP_DOWN          4   /* Decrease level by 1 (instant) */
#define DALI_CMD_RECALL_MAX         5   /* Set to maxLevel */
#define DALI_CMD_RECALL_MIN         6   /* Set to minLevel */
#define DALI_CMD_STEP_DOWN_OFF      7   /* Step down, go to OFF at minLevel */
#define DALI_CMD_ON_STEP_UP         8   /* If OFF → minLevel; else step up */
#define DALI_CMD_GO_TO_SCENE_BASE   16  /* 16–31: GO TO SCENE 0–15 */

/* Configuration commands (32–128, require config repeat) */
#define DALI_CMD_RESET              32  /* Reset all variables to defaults */
#define DALI_CMD_STORE_ACTUAL_DTR0  33  /* Store actualLevel in DTR0 */
#define DALI_CMD_DTR_AS_MAX_LEVEL   42  /* Store DTR0 as maxLevel */
#define DALI_CMD_DTR_AS_MIN_LEVEL   43  /* Store DTR0 as minLevel */
#define DALI_CMD_DTR_AS_POWER_ON    44  /* Store DTR0 as powerOnLevel */
#define DALI_CMD_DTR_AS_SYS_FAIL   45  /* Store DTR0 as systemFailureLevel */
#define DALI_CMD_DTR_AS_FADE_TIME   46  /* Store DTR0 as fadeTime (0–15) */
#define DALI_CMD_DTR_AS_FADE_RATE   47  /* Store DTR0 as fadeRate (1–15) */
#define DALI_CMD_DTR_AS_SHORT_ADDR  48  /* Store DTR0 as short address */
#define DALI_CMD_STORE_SCENE_BASE   64  /* 64–79: Store DTR0 as scene 0–15 level */
#define DALI_CMD_REMOVE_SCENE_BASE  80  /* 80–95: Remove from scene 0–15 */
#define DALI_CMD_ADD_GROUP_BASE     96  /* 96–111: Add to group 0–15 */
#define DALI_CMD_REMOVE_GROUP_BASE  112 /* 112–127: Remove from group 0–15 */
#define DALI_CMD_DTR_AS_EXT_FADE   128 /* Store DTR0 as extended fade time (DALI-2) */

/* Query commands (144–255) — verified against IEC 62386-102 / OpenKNX Commands.h */
#define DALI_CMD_QUERY_STATUS       144 /* Returns status byte */
#define DALI_CMD_QUERY_GEAR_PRESENT 145 /* Returns YES (0xFF) if gear connected */
#define DALI_CMD_QUERY_LAMP_FAILURE 146 /* YES if lamp failure (never for LED) */
#define DALI_CMD_QUERY_LAMP_POWER_ON 147 /* YES if lamp is on (actualLevel > 0) */
#define DALI_CMD_QUERY_LIMIT_ERROR  148 /* YES if level was clamped */
#define DALI_CMD_QUERY_RESET_STATE  149 /* YES if all vars at reset defaults */
#define DALI_CMD_QUERY_MISSING_SHORT 150 /* YES if no short address assigned */
#define DALI_CMD_QUERY_VERSION      151 /* Returns version number */
#define DALI_CMD_QUERY_DTR0         152 /* Returns DTR0 content */
#define DALI_CMD_QUERY_DEVICE_TYPE  153 /* Returns device type number */
#define DALI_CMD_QUERY_PHYS_MIN     154 /* Returns physical minimum level */
#define DALI_CMD_QUERY_DTR1         155 /* Returns DTR1 content */
#define DALI_CMD_QUERY_DTR2         156 /* Returns DTR2 content */
#define DALI_CMD_QUERY_ACTUAL_LEVEL 160 /* Returns actualLevel (0–254) */
#define DALI_CMD_QUERY_MAX_LEVEL    161 /* Returns maxLevel */
#define DALI_CMD_QUERY_MIN_LEVEL    162 /* Returns minLevel */
#define DALI_CMD_QUERY_POWER_ON     163 /* Returns powerOnLevel */
#define DALI_CMD_QUERY_SYS_FAIL    164 /* Returns systemFailureLevel */
#define DALI_CMD_QUERY_FADE_SPEEDS  165 /* Returns (fadeTime<<4)|fadeRate */
#define DALI_CMD_QUERY_SCENE_BASE   176 /* 176–191: Returns scene 0–15 level */
#define DALI_CMD_QUERY_RANDOM_H     194 /* Returns randomAddress byte H */
#define DALI_CMD_QUERY_RANDOM_M     195 /* Returns randomAddress byte M */
#define DALI_CMD_QUERY_RANDOM_L     196 /* Returns randomAddress byte L */
#define DALI_CMD_QUERY_GROUPS_0_7   198 /* Returns group membership bits 0–7 */
#define DALI_CMD_QUERY_GROUPS_8_15  199 /* Returns group membership bits 8–15 */

/* ── DT8 extended commands (IEC 62386-209, device type 8) ──────────
 * These are "application extended commands" (cmd 224–254), only
 * processed when preceded by ENABLE DEVICE TYPE 8 (0xC1, data=8).
 * ──────────────────────────────────────────────────────────────────*/

/* DT8 set commands (224–238) */
#define DALI_DT8_ACTIVATE                   226 /* Commit temp colour → actual */
#define DALI_DT8_SET_TEMP_COLOUR_TEMP       231 /* Tc in mirek from DTR1:DTR0 */
#define DALI_DT8_STEP_COOLER                232 /* Decrease mirek (increase K) */
#define DALI_DT8_STEP_WARMER                233 /* Increase mirek (decrease K) */
#define DALI_DT8_SET_TEMP_RGB_LEVEL         235 /* R=DTR2, G=DTR1, B=DTR0 */
#define DALI_DT8_SET_TEMP_WAF_LEVEL         236 /* W=DTR2, A=DTR1, F=DTR0 */
#define DALI_DT8_COPY_REPORT_TO_TEMP        238 /* Copy actual → temp colour */

/* DT8 query commands (247–252) */
#define DALI_DT8_QUERY_GEAR_FEATURES        247 /* Gear feature bits */
#define DALI_DT8_QUERY_COLOUR_STATUS        248 /* Colour status byte */
#define DALI_DT8_QUERY_COLOUR_TYPE_FEATURES 249 /* Supported colour types */
#define DALI_DT8_QUERY_COLOUR_VALUE         250 /* Colour value (depends on DTR0) */
#define DALI_DT8_QUERY_RGBWAF_CONTROL       251 /* Active RGBWAF channel mask */
#define DALI_DT8_QUERY_ASSIGNED_COLOUR      252 /* Assigned colour / channel info */

/* DT8 colour type feature bits (returned by QUERY_COLOUR_TYPE_FEATURES) */
#define DALI_DT8_COLOUR_TYPE_XY         0x01 /* CIE xy chromaticity */
#define DALI_DT8_COLOUR_TYPE_TC         0x02 /* Colour temperature Tc */
#define DALI_DT8_COLOUR_TYPE_PRIMARY    0x04 /* Primary N dimming */

/* DT8 Tc step size in mirek for STEP_COOLER / STEP_WARMER */
#define DALI_DT8_TC_STEP_MIREK         10

/* ── DALI special command address bytes (IEC 62386-102 §9.6) ─────── *
 * Special commands use address byte patterns 101CCCC1 (0xA1–0xBF)
 * and 110CCCC1 (0xC1–0xDF). These are NOT addressed to individual
 * devices — all control gear must process them.
 * ──────────────────────────────────────────────────────────────────*/
#define DALI_SPECIAL_TERMINATE      0xA1 /* Leave initialisation state */
#define DALI_SPECIAL_DTR            0xA3 /* Set DTR0 (data transfer reg) */
#define DALI_SPECIAL_INITIALISE     0xA5 /* Enter initialisation (send 2×) */
#define DALI_SPECIAL_RANDOMISE      0xA7 /* Generate random address (send 2×) */
#define DALI_SPECIAL_COMPARE        0xA9 /* random ≤ search → YES */
#define DALI_SPECIAL_WITHDRAW       0xAB /* random == search → withdraw */
#define DALI_SPECIAL_SEARCHADDRH    0xB1 /* Set searchAddress high byte */
#define DALI_SPECIAL_SEARCHADDRM    0xB3 /* Set searchAddress mid byte */
#define DALI_SPECIAL_SEARCHADDRL    0xB5 /* Set searchAddress low byte */
#define DALI_SPECIAL_PROGRAM_SHORT  0xB7 /* Assign short address */
#define DALI_SPECIAL_VERIFY_SHORT   0xB9 /* Verify short address → YES */
#define DALI_SPECIAL_QUERY_SHORT    0xBB /* Query short address → response */
#define DALI_SPECIAL_ENABLE_DT      0xC1 /* Enable device type features */
#define DALI_SPECIAL_DTR1           0xC3 /* Set DTR1 */
#define DALI_SPECIAL_DTR2           0xC5 /* Set DTR2 */

/* ── Initialisation timeout ──────────────────────────────────────── *
 * IEC 62386-102 §9.6.3: The control gear shall leave the
 * initialisation state no later than 15 minutes after receiving
 * the last INITIALISE command.
 * ──────────────────────────────────────────────────────────────────*/
#define DALI_INIT_TIMEOUT_MS        900000UL    /* 15 min in ms */

/* ── Fade time table (IEC 62386-102 §9.5) ──────────────────────────
 * Total transition time in milliseconds for fadeTime values 0–15.
 * Formula: T = 500 × sqrt(2^n) ms (each step × sqrt(2) ≈ 1.414).
 * fadeTime=0 means instant (no fade).
 * ──────────────────────────────────────────────────────────────────*/
static const uint32_t dali_fade_time_ms[16] = {
       0,   707,  1000,  1414,  2000,  2828,  4000,  5657,
    8000, 11314, 16000, 22627, 32000, 45255, 64000, 90510
};

/* ── Fade rate table (IEC 62386-102 §9.5) ──────────────────────────
 * Milliseconds per level step for fadeRate values 0–15.
 * Used by UP/DOWN commands for continuous fading.
 * Formula: ms = 1000 / (506 / sqrt(2^n)), fadeRate=0 is reserved.
 * ──────────────────────────────────────────────────────────────────*/
static const uint16_t dali_fade_rate_ms[16] = {
    0, 3, 4, 6, 8, 11, 16, 22, 32, 45, 63, 89, 127, 179, 250, 357
};

#endif
