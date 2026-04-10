/*
    dali_dt8.c - DT8 colour control (IEC 62386-209)

    RGBWAF primaries + colour temperature Tc. Handles SET_TEMP_RGB/WAF,
    ACTIVATE, SET_TEMP_COLOUR_TEMPERATURE, STEP_COOLER/WARMER,
    COPY_REPORT_TO_TEMP, and DT8 query commands (247–252).
*/

#include "../../config/hardware.h"

#if EVG_HAS_DT8

#include "ch32fun.h"
#include <stdio.h>
#include "dali_dt8.h"
#include "../dali_state.h"
#include "../dali_physical.h"
#include "../phy/dali_phy.h"

/* ================================================================== *
 *  Colour temperature conversion                                      *
 * ================================================================== */

#if EVG_DT8_HAS_TC && EVG_DT8_HAS_PRIMARY
static void tc_to_rgbw(uint16_t mirek, uint8_t *rgbw) {
    if (mirek < 154) mirek = 154;
    if (mirek > 370) mirek = 370;
    uint16_t m = mirek - 154;
    rgbw[0] = 160 + (uint16_t)(94  * m) / 216;
    rgbw[1] = 210 - (uint16_t)(30  * m) / 216;
    rgbw[2] = 254 - (uint16_t)(174 * m) / 216;
    rgbw[3] = 200 + (uint16_t)(54  * m) / 216;
}
#define TC_CONVERT(mirek, buf)  tc_to_rgbw(mirek, buf)

#elif EVG_DT8_HAS_TC && !EVG_DT8_HAS_PRIMARY
static void tc_to_ww_cw(uint16_t mirek, uint8_t *out) {
    if (mirek >= 370) { out[0] = 254; out[1] = 0;   return; }
    if (mirek <= 154) { out[0] = 0;   out[1] = 254; return; }
    out[0] = (uint8_t)((uint32_t)(mirek - 154) * 254 / 216);
    out[1] = (uint8_t)((uint32_t)(370 - mirek) * 254 / 216);
}
#define TC_CONVERT(mirek, buf)  tc_to_ww_cw(mirek, buf)
#endif

/* ================================================================== *
 *  DT8 command handler                                                *
 * ================================================================== */

void dali_dt8_process_command(uint8_t cmd) {
    switch (cmd) {
    case DALI_DT8_ACTIVATE:
        for (uint8_t i = 0; i < 4; i++)
            ds.colour_actual[i] = ds.colour_temp[i];
        if (ds.colour_callback)
            ds.colour_callback((const uint8_t *)ds.colour_actual, EVG_NUM_COLOURS);
        printf("DT8 ACT R=%d G=%d B=%d W=%d\n",
               ds.colour_actual[0], ds.colour_actual[1],
               ds.colour_actual[2], ds.colour_actual[3]);
        break;

#if EVG_DT8_HAS_PRIMARY
    case DALI_DT8_SET_TEMP_RGB_LEVEL:
        ds.colour_temp[0] = ds.dtr2;
        ds.colour_temp[1] = ds.dtr1;
        ds.colour_temp[2] = ds.dtr0;
        ds.colour_tc = 0;
        break;

#if EVG_NUM_COLOURS >= 4
    case DALI_DT8_SET_TEMP_WAF_LEVEL:
        ds.colour_temp[3] = ds.dtr2;
        break;
#endif
#endif /* EVG_DT8_HAS_PRIMARY */

#if EVG_DT8_HAS_TC
    case DALI_DT8_SET_TEMP_COLOUR_TEMP:
        ds.colour_tc = ((uint16_t)ds.dtr1 << 8) | ds.dtr0;
        TC_CONVERT(ds.colour_tc, (uint8_t *)ds.colour_temp);
        break;

    case DALI_DT8_STEP_COOLER:
        if (ds.colour_tc == 0) break;
        if (ds.colour_tc > 154 + DALI_DT8_TC_STEP_MIREK)
            ds.colour_tc -= DALI_DT8_TC_STEP_MIREK;
        else
            ds.colour_tc = 154;
        TC_CONVERT(ds.colour_tc, (uint8_t *)ds.colour_temp);
        break;

    case DALI_DT8_STEP_WARMER:
        if (ds.colour_tc == 0) break;
        if (ds.colour_tc < 370 - DALI_DT8_TC_STEP_MIREK)
            ds.colour_tc += DALI_DT8_TC_STEP_MIREK;
        else
            ds.colour_tc = 370;
        TC_CONVERT(ds.colour_tc, (uint8_t *)ds.colour_temp);
        break;
#endif /* EVG_DT8_HAS_TC */

    case DALI_DT8_COPY_REPORT_TO_TEMP:
        for (uint8_t i = 0; i < 4; i++)
            ds.colour_temp[i] = ds.colour_actual[i];
        break;

    /* DT8 query commands */
    case DALI_DT8_QUERY_GEAR_FEATURES:
        dali_phy_send_backward(0x00);
        break;

    case DALI_DT8_QUERY_COLOUR_STATUS:
        dali_phy_send_backward(
            (ds.colour_tc > 0 ? 0x01 : 0x00) |
            (ds.colour_tc == 0 ? 0x04 : 0x00));
        break;

    case DALI_DT8_QUERY_COLOUR_TYPE_FEATURES:
        dali_phy_send_backward(
            (EVG_DT8_HAS_TC ? DALI_DT8_COLOUR_TYPE_TC : 0) |
            (EVG_DT8_HAS_PRIMARY ? DALI_DT8_COLOUR_TYPE_PRIMARY : 0));
        break;

    case DALI_DT8_QUERY_COLOUR_VALUE:
        if (ds.dtr0 == 64) {
#if EVG_DT8_HAS_PRIMARY
            dali_phy_send_backward(EVG_NUM_COLOURS);
#else
            dali_phy_send_backward(0);
#endif
        } else if (ds.dtr0 >= 240 && ds.dtr0 < 240 + EVG_NUM_COLOURS) {
            dali_phy_send_backward(ds.colour_actual[ds.dtr0 - 240]);
        } else {
            dali_phy_send_backward(0xFF);
        }
        break;

    case DALI_DT8_QUERY_RGBWAF_CONTROL:
#if EVG_DT8_HAS_PRIMARY
        dali_phy_send_backward((1 << EVG_NUM_COLOURS) - 1);
#else
        dali_phy_send_backward(0);
#endif
        break;

    case DALI_DT8_QUERY_ASSIGNED_COLOUR:
        dali_phy_send_backward(0xFF);
        break;

    default:
        break;
    }
}

#endif /* EVG_HAS_DT8 */
