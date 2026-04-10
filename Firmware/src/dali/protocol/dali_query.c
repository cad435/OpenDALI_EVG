/*
    dali_query.c - DALI query command handler (IEC 62386-102, cmd 144-199)

    Read-only lookups on shared device state with backward frame responses.
    Extracted from dali_protocol.c for modularity.
*/

#include "dali_query.h"
#include "../dali_state.h"
#include "../dali_physical.h"
#include "../phy/dali_phy.h"
#include "dali_fade.h"
#include "dali_addressing.h"
#include "../nvm/dali_bank0.h"
#include "../../config/hardware.h"

void dali_query_process(uint8_t cmd) {
    switch (cmd) {
    case DALI_CMD_QUERY_STATUS:
        dali_phy_send_backward(
            (ds.actual_level > 0 ? 0x04 : 0x00) |
            (dali_fade_is_running() ? 0x10 : 0x00) |
            (ds.reset_state ? 0x20 : 0x00) |
            (ds.short_address == 0xFF ? 0x40 : 0x00) |
            (ds.power_cycle_seen ? 0x80 : 0x00));
        break;

    case DALI_CMD_QUERY_GEAR_PRESENT:
        dali_phy_send_backward(0xFF);
        break;

    case DALI_CMD_QUERY_LAMP_FAILURE:
        break;

    case DALI_CMD_QUERY_LAMP_POWER_ON:
        if (ds.actual_level > 0) dali_phy_send_backward(0xFF);
        break;

    case DALI_CMD_QUERY_LIMIT_ERROR:
        break;

    case DALI_CMD_QUERY_RESET_STATE:
        if (ds.reset_state) dali_phy_send_backward(0xFF);
        break;

    case DALI_CMD_QUERY_MISSING_SHORT:
        if (ds.short_address == 0xFF) {
            dali_phy_send_backward(0xFF);
        }
        break;

    case DALI_CMD_QUERY_ACTUAL_LEVEL:
        dali_phy_send_backward(ds.actual_level);
        break;

    case DALI_CMD_QUERY_MAX_LEVEL:
        dali_phy_send_backward(ds.max_level);
        break;

    case DALI_CMD_QUERY_MIN_LEVEL:
        dali_phy_send_backward(ds.min_level);
        break;

    case DALI_CMD_QUERY_POWER_ON:
        dali_phy_send_backward(ds.power_on_level);
        break;

    case DALI_CMD_QUERY_SYS_FAIL:
        dali_phy_send_backward(ds.sys_fail_level);
        break;

    case DALI_CMD_QUERY_DTR1:
        dali_phy_send_backward(ds.dtr1);
        break;

    case DALI_CMD_QUERY_DTR2:
        dali_phy_send_backward(ds.dtr2);
        break;

    case DALI_CMD_QUERY_GROUPS_0_7:
        dali_phy_send_backward(ds.group_membership & 0xFF);
        break;

    case DALI_CMD_QUERY_GROUPS_8_15:
        dali_phy_send_backward((ds.group_membership >> 8) & 0xFF);
        break;

    case DALI_CMD_QUERY_RANDOM_H:
        dali_phy_send_backward(dali_addressing_random_h());
        break;

    case DALI_CMD_QUERY_RANDOM_M:
        dali_phy_send_backward(dali_addressing_random_m());
        break;

    case DALI_CMD_QUERY_RANDOM_L:
        dali_phy_send_backward(dali_addressing_random_l());
        break;

    case DALI_CMD_READ_MEMORY:
        if (ds.dtr2 == 0 && ds.dtr1 <= DALI_BANK0_LAST_ADDR) {
            uint8_t value = dali_bank0_read(ds.dtr1);
            dali_phy_send_backward(value);
            ds.dtr0 = value;
            if (ds.dtr1 < 0xFF) ds.dtr1++;
        }
        break;

    case DALI_CMD_QUERY_VERSION:
        dali_phy_send_backward(1);
        break;

    case DALI_CMD_QUERY_DTR0:
        dali_phy_send_backward(ds.dtr0);
        break;

    case DALI_CMD_QUERY_DEVICE_TYPE:
        dali_phy_send_backward(DALI_DEVICE_TYPE);
        break;

    case DALI_CMD_QUERY_PHYS_MIN:
#ifdef ONOFF_MODE
        dali_phy_send_backward(254);
#else
        dali_phy_send_backward(1);
#endif
        break;

    case DALI_CMD_QUERY_FADE_SPEEDS:
        dali_phy_send_backward((ds.fade_time << 4) | ds.fade_rate);
        break;

    default:
        if (cmd >= DALI_CMD_QUERY_SCENE_BASE && cmd <= DALI_CMD_QUERY_SCENE_BASE + 15) {
            dali_phy_send_backward(ds.scene_level[cmd - DALI_CMD_QUERY_SCENE_BASE]);
        }
        break;
    }
}
