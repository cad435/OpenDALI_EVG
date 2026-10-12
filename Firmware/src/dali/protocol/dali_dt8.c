/*
    dali_dt8.c - DT8 colour control (IEC 62386-209)

    RGBWAF primaries + colour temperature Tc. Handles SET_TEMP_RGB/WAF,
    SET_TEMP_RGBWAF_CONTROL, ACTIVATE, SET_TEMP_COLOUR_TEMPERATURE,
    STEP_COOLER/WARMER, COPY_REPORT_TO_TEMP, and DT8 query commands
    (247–252, 255).

    Colour types: RGBWAF (channel control, all channels linked) when
    EVG_DT8_HAS_PRIMARY, colour temperature Tc when EVG_DT8_HAS_TC.
    xy-coordinate and primary N are not supported.

    Temporary colour setting (209 §9.12.4): SET TEMPORARY ... stages values
    and the temporary colour type; 0xFF / 0xFFFF = MASK (no change).
    ACTIVATE applies them and resets the temporaries to MASK.
    The active colour type is derived from ds.colour_tc: non-zero = Tc.
*/

#include "../../config/hardware.h"

#if EVG_HAS_DT8

#include "ch32fun.h"
#include "dali_dt8.h"
#include "dali_fade.h"
#include "../../logger.h"
#include "../dali_state.h"
#include "../dali_dtr.h"
#include "../dali_physical.h"
#include "../phy/dali_phy.h"

#define MASK8   0xFF
#define MASK16  0xFFFF

/* ================================================================== *
 *  Colour temperature conversion                                      *
 * ================================================================== */

#if EVG_DT8_HAS_TC && EVG_DT8_HAS_PRIMARY
static void tc_to_rgbw(uint16_t mirek, uint8_t *rgbw) {
    uint16_t m = mirek - DALI_DT8_TC_COOLEST;
    rgbw[0] = 160 + (uint16_t)(94  * m) / 216;
    rgbw[1] = 210 - (uint16_t)(30  * m) / 216;
    rgbw[2] = 254 - (uint16_t)(174 * m) / 216;
    rgbw[3] = 200 + (uint16_t)(54  * m) / 216;
}
#define TC_CONVERT(mirek, buf)  tc_to_rgbw(mirek, buf)

#elif EVG_DT8_HAS_TC && !EVG_DT8_HAS_PRIMARY
static void tc_to_ww_cw(uint16_t mirek, uint8_t *out) {
    out[0] = (uint8_t)((uint32_t)(mirek - DALI_DT8_TC_COOLEST) * 254 / 216);
    out[1] = (uint8_t)((uint32_t)(DALI_DT8_TC_WARMEST - mirek) * 254 / 216);
}
#define TC_CONVERT(mirek, buf)  tc_to_ww_cw(mirek, buf)
#endif

/* ================================================================== *
 *  Helpers                                                            *
 * ================================================================== */

static uint8_t active_colour_type(void) {
#if EVG_DT8_HAS_PRIMARY
    return ds.colour_tc ? DALI_DT8_TYPE_TC : DALI_DT8_TYPE_RGBWAF;
#else
    return DALI_DT8_TYPE_TC;
#endif
}

static void clear_temp(void) {
    for (uint8_t i = 0; i < 4; i++)
        ds.colour_temp[i] = MASK8;
    ds.colour_tc_temp   = MASK16;
    ds.colour_type_temp = MASK8;
}

/* 16-bit answer (209 §9.9): MSB → DTR1, LSB → DTR0, answer = MSB. */
static void answer_16(uint16_t value) {
    ds.dtr1 = value >> 8;
    ds.dtr0 = value & 0xFF;
    dali_phy_send_backward(ds.dtr1);
}

#if EVG_DT8_HAS_TC
/* Active Tc as reported by QUERY COLOUR VALUE 2 / 226 — MASK when the
 * active colour type is not Tc (RGBW → Tc cannot be recalculated). */
static uint16_t reported_tc(void) {
    return ds.colour_tc ? ds.colour_tc : MASK16;
}

/* Apply a Tc immediately (no fade), used by STEP COOLER / WARMER. */
static void apply_tc_now(uint16_t mirek) {
    uint8_t target[4];
    for (uint8_t i = 0; i < 4; i++)
        target[i] = ds.colour_actual[i];
    ds.colour_tc = mirek;
    TC_CONVERT(mirek, target);
    dali_fade_colour_start(target, 0);
}
#endif

#if EVG_DT8_HAS_PRIMARY
/* RGBWAF CONTROL: channel control, every present channel linked. */
#define RGBWAF_CONTROL  ((1 << EVG_NUM_COLOURS) - 1)
#endif

void dali_dt8_reset(void) {
    clear_temp();
    ds.colour_tc     = 0;
    ds.colour_status = 0;
}

/* QUERY COLOUR VALUE (209 cmd 250, Table 11). Selector in DTR0.
 * Unsupported selectors / colour types get no answer at all. */
static void query_colour_value(void) {
    uint8_t sel = ds.dtr0;

#if EVG_DT8_HAS_TC
    switch (sel) {
    case 2:     /* COLOUR TEMPERATURE Tc */
    case 226:   /* REPORT COLOUR TEMPERATURE Tc */
        answer_16(reported_tc());
        return;
    case 128:   /* Tc COOLEST */
    case 129:   /* Tc PHYSICAL COOLEST */
        answer_16(DALI_DT8_TC_COOLEST);
        return;
    case 130:   /* Tc WARMEST */
    case 131:   /* Tc PHYSICAL WARMEST */
        answer_16(DALI_DT8_TC_WARMEST);
        return;
    case 194:   /* TEMPORARY COLOUR TEMPERATURE Tc */
        answer_16(ds.colour_tc_temp);
        return;
    }
#endif

#if EVG_DT8_HAS_PRIMARY
    if (sel >= 9 && sel <= 14) {            /* RED … FREECOLOUR DIMLEVEL */
        uint8_t i = sel - 9;
        dali_phy_send_backward(i < EVG_NUM_COLOURS ? ds.colour_actual[i] : MASK8);
        return;
    }
    if (sel >= 201 && sel <= 206) {         /* TEMPORARY RED … FREECOLOUR */
        uint8_t i = sel - 201;
        dali_phy_send_backward(i < EVG_NUM_COLOURS ? ds.colour_temp[i] : MASK8);
        return;
    }
    if (sel >= 233 && sel <= 238) {         /* REPORT RED … FREECOLOUR */
        uint8_t i = sel - 233;
        dali_phy_send_backward((i < EVG_NUM_COLOURS && !ds.colour_tc)
                               ? ds.colour_actual[i] : MASK8);
        return;
    }
    if (sel == 15) {                        /* RGBWAF CONTROL */
        dali_phy_send_backward(RGBWAF_CONTROL);
        return;
    }
    if (sel == 207) {                       /* TEMPORARY RGBWAF CONTROL */
        dali_phy_send_backward(MASK8);
        return;
    }
    if (sel == 239) {                       /* REPORT RGBWAF CONTROL */
        dali_phy_send_backward(ds.colour_tc ? MASK8 : RGBWAF_CONTROL);
        return;
    }
#endif

    if (sel == 208) {                       /* TEMPORARY COLOUR TYPE */
        dali_phy_send_backward(ds.colour_type_temp);
    } else if (sel == 240) {                /* REPORT COLOUR TYPE */
        dali_phy_send_backward(active_colour_type());
    }
}

/* ================================================================== *
 *  DT8 command handler                                                *
 * ================================================================== */

void dali_dt8_process_command(uint8_t cmd) {
    switch (cmd) {
    case DALI_DT8_ACTIVATE: {
        /* Crossfade the active colour toward the staged colour over the
         * current fade time (IEC 62386-209 §9.12.5 — colour follows fadeTime).
         * MASK values leave their channel unchanged; a MASK colour type
         * means no colour change at all. */
        if (ds.colour_type_temp == MASK8) break;

        uint8_t target[4];
        for (uint8_t i = 0; i < 4; i++)
            target[i] = ds.colour_actual[i];

#if EVG_DT8_HAS_TC
        if (ds.colour_type_temp == DALI_DT8_TYPE_TC && ds.colour_tc_temp != MASK16) {
            uint16_t tc = ds.colour_tc_temp;
            ds.colour_status = 0;
            if (tc < DALI_DT8_TC_COOLEST || tc > DALI_DT8_TC_WARMEST) {
                ds.colour_status = DALI_DT8_STATUS_TC_OUT_OF_RANGE;
                tc = (tc < DALI_DT8_TC_COOLEST) ? DALI_DT8_TC_COOLEST : DALI_DT8_TC_WARMEST;
            }
            ds.colour_tc = tc;
            TC_CONVERT(tc, target);
        }
#endif
#if EVG_DT8_HAS_PRIMARY
        if (ds.colour_type_temp == DALI_DT8_TYPE_RGBWAF) {
            for (uint8_t i = 0; i < EVG_NUM_COLOURS; i++)
                if (ds.colour_temp[i] != MASK8)
                    target[i] = ds.colour_temp[i];
            ds.colour_tc     = 0;
            ds.colour_status = 0;
        }
#endif
        clear_temp();
        /* fadeTime 0 → applied instantly inside dali_fade_colour_start(). */
        dali_fade_colour_start(target, dali_fade_get_effective_ms());
        LOG_CMD("DT8 ACT R=%d G=%d B=%d W=%d Tc=%d (fade %lums)",
                target[0], target[1], target[2], target[3], ds.colour_tc,
                (unsigned long)dali_fade_get_effective_ms());
        break;
    }

#if EVG_DT8_HAS_PRIMARY
    case DALI_DT8_SET_TEMP_RGB_LEVEL:
        ds.colour_temp[0] = ds.dtr0;
        ds.colour_temp[1] = ds.dtr1;
        ds.colour_temp[2] = ds.dtr2;
        ds.colour_tc_temp   = MASK16;
        ds.colour_type_temp = DALI_DT8_TYPE_RGBWAF;
        break;

    case DALI_DT8_SET_TEMP_WAF_LEVEL:
#if EVG_NUM_COLOURS >= 4
        ds.colour_temp[3] = ds.dtr0;
#endif
        ds.colour_tc_temp   = MASK16;
        ds.colour_type_temp = DALI_DT8_TYPE_RGBWAF;
        break;

    case DALI_DT8_SET_TEMP_RGBWAF_CONTROL:
        /* Channel linking is fixed (all linked, channel control); the
         * command only selects RGBWAF as the temporary colour type. */
        ds.colour_tc_temp   = MASK16;
        ds.colour_type_temp = DALI_DT8_TYPE_RGBWAF;
        break;
#endif /* EVG_DT8_HAS_PRIMARY */

#if EVG_DT8_HAS_TC
    case DALI_DT8_SET_TEMP_COLOUR_TEMP: {
        uint16_t tc = dtr_colour_temp();
        if (tc == 0) break;                 /* 209 cmd 231: 0 is ignored */
        for (uint8_t i = 0; i < 4; i++)
            ds.colour_temp[i] = MASK8;
        ds.colour_tc_temp   = tc;
        ds.colour_type_temp = DALI_DT8_TYPE_TC;
        break;
    }

    case DALI_DT8_STEP_COOLER:
        /* Only with Tc active; 1 mirek immediately, no fade (209 cmd 232). */
        if (ds.colour_tc && ds.colour_tc > DALI_DT8_TC_COOLEST)
            apply_tc_now(ds.colour_tc - 1);
        break;

    case DALI_DT8_STEP_WARMER:
        if (ds.colour_tc && ds.colour_tc < DALI_DT8_TC_WARMEST)
            apply_tc_now(ds.colour_tc + 1);
        break;
#endif /* EVG_DT8_HAS_TC */

    case DALI_DT8_COPY_REPORT_TO_TEMP:
        clear_temp();
        ds.colour_type_temp = active_colour_type();
#if EVG_DT8_HAS_TC
        if (ds.colour_type_temp == DALI_DT8_TYPE_TC)
            ds.colour_tc_temp = reported_tc();
#endif
#if EVG_DT8_HAS_PRIMARY
        if (ds.colour_type_temp == DALI_DT8_TYPE_RGBWAF)
            for (uint8_t i = 0; i < EVG_NUM_COLOURS; i++)
                ds.colour_temp[i] = ds.colour_actual[i];
#endif
        break;

    /* DT8 query commands */
    case DALI_DT8_QUERY_GEAR_FEATURES:
        dali_phy_send_backward(0x00);       /* no auto activation / calibration */
        break;

    case DALI_DT8_QUERY_COLOUR_STATUS:
        dali_phy_send_backward(ds.colour_status | active_colour_type());
        break;

    case DALI_DT8_QUERY_COLOUR_TYPE_FEATURES:
        dali_phy_send_backward(
            (EVG_DT8_HAS_TC ? DALI_DT8_FEATURE_TC : 0) |
            (EVG_DT8_HAS_PRIMARY ? DALI_DT8_FEATURE_RGBWAF(EVG_NUM_COLOURS) : 0));
        break;

    case DALI_DT8_QUERY_COLOUR_VALUE:
        query_colour_value();
        break;

#if EVG_DT8_HAS_PRIMARY
    case DALI_DT8_QUERY_RGBWAF_CONTROL:
        dali_phy_send_backward(RGBWAF_CONTROL);
        break;

    case DALI_DT8_QUERY_ASSIGNED_COLOUR:
        /* Channel n carries colour n+1 (1 = red … 4 = white, Table 12). */
        dali_phy_send_backward(ds.dtr0 < EVG_NUM_COLOURS ? ds.dtr0 + 1 : MASK8);
        break;
#endif

    case DALI_DT8_QUERY_EXTENDED_VERSION:
        dali_phy_send_backward(2);
        break;

    default:
        break;
    }
}

#endif /* EVG_HAS_DT8 */
