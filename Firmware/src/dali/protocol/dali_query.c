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
#include "../nvm/dali_bank2.h"
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

    case DALI_CMD_READ_MEMORY: {
        /* IEC 62386-102:2009 §9.8 / Command 197:
         *   - memory bank is selected by DTR1
         *   - address within bank is given by DTR0 (auto-incremented after read)
         *   - DTR2 receives the next byte (look-ahead), if still in range
         * We implement bank 0 (identification) and bank 2 (build identity). */
        uint8_t last;
        uint8_t (*bank_read)(uint8_t);
        if (ds.dtr1 == 0) {
            last = DALI_BANK0_LAST_ADDR;
            bank_read = dali_bank0_read;
        } else if (ds.dtr1 == DALI_BANK2_NUMBER) {
            last = DALI_BANK2_LAST_ADDR;
            bank_read = dali_bank2_read;
        } else {
            break;
        }
        if (ds.dtr0 <= last) {
            dali_phy_send_backward(bank_read(ds.dtr0));
            if (ds.dtr0 < last)
                ds.dtr2 = bank_read(ds.dtr0 + 1);
            if (ds.dtr0 < 0xFF) ds.dtr0++;
        }
        break;
    }

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
