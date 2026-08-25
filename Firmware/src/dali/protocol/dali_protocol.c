/*
    dali_protocol.c - DALI protocol handler (IEC 62386-102)

    Command dispatcher, query handler, config commands, arc power,
    NVM state serialization. Orchestrates sub-modules:
    dali_phy, dali_fade, dali_addressing, dali_dt8.
*/

#include "ch32fun.h"
#include <stdio.h>
#include "dali_protocol.h"
#include "../../logger.h"
#include "../dali_state.h"
#include "../dali_physical.h"
#include "../dali_frame.h"
#include "../phy/dali_phy.h"
#include "dali_fade.h"
#include "dali_addressing.h"
#include "dali_dt8.h"
#include "dali_query.h"
#include "dali_config_repeat.h"
#include "dali_cmd_scenes.h"
#include "../dali_dtr.h"
#include "../nvm/dali_nvm.h"

/* millis() provided by main.c */
extern uint32_t millis(void);

/* ── Global shared device state instance ─────────────────────────── */
dali_device_state_t ds = {
    .actual_level    = 0,
    .max_level       = 254,
#ifdef ONOFF_MODE
    .min_level       = 254,
#else
    .min_level       = 1,
#endif
    .power_on_level  = 254,
    .sys_fail_level  = 254,
    .fade_time       = 0,
    .fade_rate        = 7,
    .ext_fade_base   = 0,
    .ext_fade_mult   = 0,
    .short_address   = 0xFF,
    .group_membership = 0,
    .scene_level     = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                         0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
    .dtr0            = 0,
    .dtr1            = 0,
    .dtr2            = 0,
    .enabled_device_type = 0xFF,
    .colour_actual   = {254, 254, 254, 254},
#if EVG_HAS_DT8
    .colour_temp     = {254, 254, 254, 254},
    .colour_tc       = 0,
#endif
    .reset_state     = 1,
    .power_cycle_seen = 1,
    .arc_callback    = 0,
    .colour_callback = 0,
};

/* ── DALI bootloader entry via software reset ─────────────────────── */
static void enter_bootloader(void) {
    /* WCH official SystemReset_StartMode(Start_Mode_BOOT) sequence.
     * Copied from LED-Snowflake bootloader.h which works on CH32V003.
     * Sets FLASH->STATR bit 14 which the bootloader checks on startup. */
    __disable_irq();
    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;
    FLASH->MODEKEYR = FLASH_KEY2;
    FLASH->BOOT_MODEKEYR = FLASH_KEY1;
    FLASH->BOOT_MODEKEYR = FLASH_KEY2;
    FLASH->STATR &= ~(1 << 14);
    FLASH->STATR |= (1 << 14);
    FLASH->CTLR = CR_LOCK_Set;
    PFIC->SCTLR = 1 << 31;
    while (1);
}

/* Config repeat validation — shared implementation in dali_config_repeat.c.
 * Local alias for readability within this file. */
#define check_config_repeat dali_check_config_repeat

/* ================================================================== *
 *  process_frame() — dispatch a received 16-bit forward frame         *
 * ================================================================== */
static void process_frame(const dali_frame_t *frame) {
    if (frame->flags & (DALI_FRAME_FLAG_ERROR | DALI_FRAME_FLAG_ECHO)) return;
    if (frame->size != 16) return;

    uint8_t addr_byte = dali_frame_addr_byte(frame);
    uint8_t data_byte = dali_frame_data_byte(frame);

    /* Special command detection */
    uint8_t top = addr_byte & 0xE1;
    if (top == 0xA1 || top == 0xC1) {
        dali_addressing_process_special(addr_byte, data_byte);
        return;
    }

    /* Consume ENABLE_DT state */
#if EVG_HAS_DT8
    uint8_t enabled_dt = ds.enabled_device_type;
#endif
    ds.enabled_device_type = 0xFF;

    if (!is_addressed_to_me(addr_byte)) return;

    uint8_t S = addr_byte & 1;

    if (S == 0) {
        /* ── Direct arc power command ─────────────────────────────── */
        if (data_byte == 0xFF) return;
        dali_fade_stop();
        ds.power_cycle_seen = 0;  /* IEC 62386-102 §9.16.9 */
        uint8_t level = clamp_level(data_byte);
        uint32_t eff_fade_ms = dali_fade_get_effective_ms();

        if (level == 0 || eff_fade_ms == 0 || ds.actual_level == level) {
            ds.actual_level = level;
            if (ds.arc_callback) ds.arc_callback(level);
        } else {
            dali_fade_start(level, eff_fade_ms);
        }
    } else {
        /* ── Command dispatch (S=1) ──────────────────────────────── */
        uint32_t now = millis();

        switch (data_byte) {
        /* Immediate action commands */
        case DALI_CMD_OFF:
            dali_fade_stop();
            ds.power_cycle_seen = 0;  /* IEC 62386-102 §9.16.9 */
            ds.actual_level = 0;
            if (ds.arc_callback) ds.arc_callback(0);
            break;

        case DALI_CMD_UP:
            dali_fade_stop();
            ds.power_cycle_seen = 0;  /* IEC 62386-102 §9.16.9 */
            if (ds.actual_level >= ds.max_level) break;
            if (ds.actual_level == 0) {
                ds.actual_level = ds.min_level;
                if (ds.arc_callback) ds.arc_callback(ds.min_level);
            }
            dali_fade_start_rate(ds.max_level, dali_fade_rate_ms[ds.fade_rate]);
            break;

        case DALI_CMD_DOWN:
            dali_fade_stop();
            ds.power_cycle_seen = 0;  /* IEC 62386-102 §9.16.9 */
            if (ds.actual_level == 0 || ds.actual_level <= ds.min_level) break;
            dali_fade_start_rate(ds.min_level, dali_fade_rate_ms[ds.fade_rate]);
            break;

        case DALI_CMD_STEP_UP:
            dali_fade_stop();
            ds.power_cycle_seen = 0;  /* IEC 62386-102 §9.16.9 */
            if (ds.actual_level == 0) ds.actual_level = ds.min_level;
            else if (ds.actual_level < ds.max_level) ds.actual_level++;
            else break;
            if (ds.arc_callback) ds.arc_callback(ds.actual_level);
            break;

        case DALI_CMD_STEP_DOWN:
            dali_fade_stop();
            ds.power_cycle_seen = 0;  /* IEC 62386-102 §9.16.9 */
            if (ds.actual_level == 0) break;
            if (ds.actual_level <= ds.min_level) {
                ds.actual_level = 0;
            } else {
                ds.actual_level--;
            }
            if (ds.arc_callback) ds.arc_callback(ds.actual_level);
            break;

        case DALI_CMD_RECALL_MAX:
            dali_fade_stop();
            ds.power_cycle_seen = 0;  /* IEC 62386-102 §9.16.9 */
            ds.actual_level = ds.max_level;
            if (ds.arc_callback) ds.arc_callback(ds.max_level);
            break;

        case DALI_CMD_RECALL_MIN:
            dali_fade_stop();
            ds.power_cycle_seen = 0;  /* IEC 62386-102 §9.16.9 */
            ds.actual_level = ds.min_level;
            if (ds.arc_callback) ds.arc_callback(ds.min_level);
            break;

        case DALI_CMD_STEP_DOWN_OFF:
            dali_fade_stop();
            ds.power_cycle_seen = 0;  /* IEC 62386-102 §9.16.9 */
            if (ds.actual_level == 0) break;
            if (ds.actual_level <= ds.min_level) {
                ds.actual_level = 0;
            } else {
                ds.actual_level--;
            }
            if (ds.arc_callback) ds.arc_callback(ds.actual_level);
            break;

        case DALI_CMD_ON_STEP_UP:
            dali_fade_stop();
            ds.power_cycle_seen = 0;  /* IEC 62386-102 §9.16.9 */
            if (ds.actual_level == 0) {
                ds.actual_level = ds.min_level;
            } else if (ds.actual_level < ds.max_level) {
                ds.actual_level++;
            } else {
                break;
            }
            if (ds.arc_callback) ds.arc_callback(ds.actual_level);
            break;

        /* Configuration commands (require config repeat) */
        case DALI_CMD_RESET:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                dali_fade_stop();
                ds.power_cycle_seen = 0;  /* IEC 62386-102 §9.16.9 */
                ds.actual_level = 254;
                ds.max_level = 254;
#ifdef ONOFF_MODE
                ds.min_level = 254;
#else
                ds.min_level = 1;
#endif
                ds.power_on_level = 254;
                ds.sys_fail_level = 254;
                ds.fade_time = 0;
                ds.fade_rate = 7;
                ds.ext_fade_base = 0;
                ds.ext_fade_mult = 0;
                ds.group_membership = 0;
                for (uint8_t i = 0; i < 16; i++) ds.scene_level[i] = 0xFF;
                for (uint8_t i = 0; i < 4; i++)
                    ds.colour_actual[i] = 254;
#if EVG_HAS_DT8
                dali_fade_colour_stop();
                for (uint8_t i = 0; i < 4; i++)
                    ds.colour_temp[i] = 254;
                ds.colour_tc = 0;
#endif
                ds.reset_state = 1;
                if (ds.arc_callback) ds.arc_callback(ds.actual_level);
#if EVG_HAS_DT8
                if (ds.colour_callback)
                    ds.colour_callback((const uint8_t *)ds.colour_actual, EVG_NUM_COLOURS);
#endif
                nvm_mark_dirty();
                LOG_CMD("RESET");
            }
            break;

        case DALI_CMD_STORE_ACTUAL_DTR0:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                ds.dtr0 = ds.actual_level;
            }
            break;

        case DALI_CMD_DTR_AS_MAX_LEVEL:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                ds.max_level = ds.dtr0;
                if (ds.max_level < ds.min_level) ds.max_level = ds.min_level;
                nvm_mark_dirty();
                ds.reset_state = 0;
                LOG_CMD("MAX=%d", ds.max_level);
            }
            break;

        case DALI_CMD_DTR_AS_MIN_LEVEL:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                ds.min_level = ds.dtr0;
#ifdef ONOFF_MODE
                ds.min_level = 254;
#else
                if (ds.min_level < 1) ds.min_level = 1;
#endif
                if (ds.min_level > ds.max_level) ds.min_level = ds.max_level;
                nvm_mark_dirty();
                ds.reset_state = 0;
                LOG_CMD("MIN=%d", ds.min_level);
            }
            break;

        case DALI_CMD_DTR_AS_POWER_ON:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                ds.power_on_level = ds.dtr0;
                nvm_mark_dirty();
                ds.reset_state = 0;
                LOG_CMD("PON=%d", ds.power_on_level);
            }
            break;

        case DALI_CMD_DTR_AS_SYS_FAIL:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                ds.sys_fail_level = ds.dtr0;
                nvm_mark_dirty();
                ds.reset_state = 0;
                LOG_CMD("SFAIL=%d", ds.sys_fail_level);
            }
            break;

        case DALI_CMD_DTR_AS_FADE_TIME:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                ds.fade_time = dtr_fade_time();
                nvm_mark_dirty();
                ds.reset_state = 0;
                LOG_CMD("FADE_TIME=%d", ds.fade_time);
            }
            break;

        case DALI_CMD_DTR_AS_FADE_RATE:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                uint8_t r = dtr_fade_rate();
                if (r > 0) ds.fade_rate = r;
                nvm_mark_dirty();
                ds.reset_state = 0;
                LOG_CMD("FADE_RATE=%d", ds.fade_rate);
            }
            break;

        case DALI_CMD_DTR_AS_SHORT_ADDR:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                if (ds.dtr0 == 0xFF) {
                    ds.short_address = 0xFF;
                } else {
                    ds.short_address = dtr_short_address();
                }
                nvm_mark_dirty();
                ds.reset_state = 0;
                LOG_CMD("SHORT_ADDR=%d", ds.short_address);
            }
            break;

        case 129: /* ENABLE WRITE MEMORY — not implemented, reuse for ext fade (DALI-2) */
            if (check_config_repeat(addr_byte, data_byte, now)) {
                if (ds.dtr0 > 0x4F) {
                    ds.ext_fade_base = 0;
                    ds.ext_fade_mult = 0;
                } else {
                    ds.ext_fade_base = dtr_ext_fade_base();
                    ds.ext_fade_mult = dtr_ext_fade_mult();
                }
                nvm_mark_dirty();
                ds.reset_state = 0;
                LOG_CMD("EXTFADE b=%d m=%d", ds.ext_fade_base, ds.ext_fade_mult);
            }
            break;

        case DALI_CMD_ENTER_BOOTLOADER:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                LOG_CMD("BOOTLOADER!");
                nvm_save();
                enter_bootloader();
            }
            break;

        default:
            /* Range-based commands — scenes, groups, DT8, queries */
            if (data_byte >= DALI_CMD_GO_TO_SCENE_BASE
                && data_byte <= DALI_CMD_GO_TO_SCENE_BASE + 15) {
                dali_scene_recall(data_byte - DALI_CMD_GO_TO_SCENE_BASE);
            } else if (data_byte >= DALI_CMD_STORE_SCENE_BASE
                       && data_byte <= DALI_CMD_STORE_SCENE_BASE + 15) {
                if (check_config_repeat(addr_byte, data_byte, now))
                    dali_scene_store(data_byte - DALI_CMD_STORE_SCENE_BASE);
            } else if (data_byte >= DALI_CMD_REMOVE_SCENE_BASE
                       && data_byte <= DALI_CMD_REMOVE_SCENE_BASE + 15) {
                if (check_config_repeat(addr_byte, data_byte, now))
                    dali_scene_remove(data_byte - DALI_CMD_REMOVE_SCENE_BASE);
            } else if (data_byte >= DALI_CMD_ADD_GROUP_BASE
                       && data_byte <= DALI_CMD_ADD_GROUP_BASE + 15) {
                if (check_config_repeat(addr_byte, data_byte, now))
                    dali_group_add(data_byte - DALI_CMD_ADD_GROUP_BASE);
            } else if (data_byte >= DALI_CMD_REMOVE_GROUP_BASE
                       && data_byte <= DALI_CMD_REMOVE_GROUP_BASE + 15) {
                if (check_config_repeat(addr_byte, data_byte, now))
                    dali_group_remove(data_byte - DALI_CMD_REMOVE_GROUP_BASE);
#if EVG_HAS_DT8
            } else if (data_byte >= 224 && enabled_dt == DALI_DEVICE_TYPE) {
                dali_dt8_process_command(data_byte);
#endif
            } else if (data_byte >= 144) {
                dali_query_process(data_byte);
            }
            break;
        }
    }
}

/* ================================================================== *
 *  PUBLIC API                                                         *
 * ================================================================== */

void dali_protocol_init(void) {
    /* State is initialized by the ds struct initializer above */
}

/* ── 32-bit forward frame handler (IEC 62386-105) ────────────────── */
static void process_frame_32(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    /* NOTE: the bootloader-update engine used to hook in here. Moved out
     * 2026-08-15 to Firmware/emergency_bl_flasher/ — writing the BOOT area
     * from user code requires an ACTIVE DEBUG MODULE (proven on HW: without
     * it, erase/program are silent no-ops that still set EOP). BL updates
     * are therefore an emergency procedure via a dedicated mini-firmware,
     * not a feature of the normal application. */
    /*
     * IEC 62386-105 standard commands:
     *   [addr] [0xFB] [cmd] [0x00]
     *
     * 32-bit address byte format (Table 1):
     *   Bit 7..1 = address field, Bit 0 = address space (0=gear, 1=device)
     *   Short address: (short_addr << 1) | 0  (for gear)
     *   Broadcast:     0xFE (gear), 0xFF (device)
     *   Broadcast UA:  0xFC (gear), 0xFD (device)
     */
    if (b1 != 0xFB) return;     /* only standard commands (opcode byte 1) */

    /* Address check for 32-bit frames (gear: bit 0 = 0) */
    uint8_t my_addr_32 = (ds.short_address <= 63) ? (ds.short_address << 1) : 0xFE;
    if (b0 != my_addr_32 && b0 != 0xFE && b0 != 0xFC)
        return;

    uint32_t now = millis();

    switch (b2) {
    case 0x00:  /* START FW TRANSFER — config repeat required */
        if (b3 == 0x00 && check_config_repeat(b0, b2, now)) {
            LOG_CMD("FW_TRANSFER (IEC105)!");
            dali_phy_send_backward(0xFF);    /* YES */
            nvm_save();
            enter_bootloader();
        }
        break;

    case 0x05:  /* QUERY FW UPDATE FEATURES */
        /* Bit 0 = fwUpdateCancelSupported (0 = not supported) */
        dali_phy_send_backward(0x00);
        break;
    }
}

void dali_protocol_process(void) {
    dali_addressing_check_timeout();

    if (!dali_phy_frame_ready()) return;

    uint8_t raw[4];
    dali_phy_frame_bytes(raw);
    uint8_t bitlen = dali_phy_frame_bits();

    if (bitlen == 16) {
        dali_frame_t frame = {
            .data      = ((uint32_t)raw[0] << 8) | (uint32_t)raw[1],
            .size      = 16,
            .flags     = DALI_FRAME_FLAG_FORWARD,
            .timestamp = millis(),
        };
        process_frame(&frame);
    } else if (bitlen == 32) {
        process_frame_32(raw[0], raw[1], raw[2], raw[3]);
    }
}

void dali_protocol_power_on(void) {
    uint8_t level = ds.power_on_level;
    if (level == 0xFF) level = ds.max_level;
    level = clamp_level(level);
    ds.actual_level = level;
    ds.power_cycle_seen = 1;
    if (ds.arc_callback) ds.arc_callback(ds.actual_level);
#if EVG_HAS_DT8
    if (ds.colour_callback)
        ds.colour_callback((const uint8_t *)ds.colour_actual, EVG_NUM_COLOURS);
#endif
}

void dali_protocol_set_arc_callback(dali_arc_callback_t cb) {
    ds.arc_callback = cb;
}

void dali_protocol_set_colour_callback(dali_colour_callback_t cb) {
    ds.colour_callback = cb;
}

uint8_t dali_protocol_get_actual_level(void) {
    return ds.actual_level;
}

const volatile uint8_t *dali_protocol_get_colour_actual(void) {
    return ds.colour_actual;
}

/* NVM state pack/unpack is in dali_nvm.c (reads/writes ds directly) */
