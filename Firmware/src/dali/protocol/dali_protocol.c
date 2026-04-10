/*
    dali_protocol.c - DALI protocol handler (IEC 62386-102)

    Command dispatcher, query handler, config commands, arc power,
    NVM state serialization. Orchestrates sub-modules:
    dali_phy, dali_fade, dali_addressing, dali_dt8.
*/

#include "ch32fun.h"
#include <stdio.h>
#include "dali_protocol.h"
#include "../dali_state.h"
#include "../dali_physical.h"
#include "../dali_frame.h"
#include "../phy/dali_phy.h"
#include "dali_fade.h"
#include "dali_addressing.h"
#include "dali_dt8.h"
#include "dali_query.h"
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
#define BOOTLOADER_MAGIC_ADDR   ((volatile uint32_t *)0x200007F0)
#define BOOTLOADER_MAGIC_VALUE  0xDAB00DAD

static void enter_bootloader(void) {
    *BOOTLOADER_MAGIC_ADDR = BOOTLOADER_MAGIC_VALUE;
    FLASH->BOOT_MODEKEYR = FLASH_KEY1;
    FLASH->BOOT_MODEKEYR = FLASH_KEY2;
    FLASH->STATR = 1 << 14;
    FLASH->CTLR = CR_LOCK_Set;
    PFIC->SCTLR = 1 << 31;
    while (1);
}

/* ── Config repeat validation ────────────────────────────────────── */
static volatile uint8_t     last_addr_byte = 0;
static volatile uint8_t     last_command = 0;
static volatile uint32_t    last_command_time = 0;
static volatile uint8_t     config_repeat_pending = 0;

static uint8_t check_config_repeat(uint8_t addr, uint8_t cmd, uint32_t now) {
    if (config_repeat_pending && last_addr_byte == addr
        && last_command == cmd
        && (now - last_command_time) <= 100) {
        config_repeat_pending = 0;
        return 1;
    }
    last_addr_byte = addr;
    last_command = cmd;
    last_command_time = now;
    config_repeat_pending = 1;
    return 0;
}

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
            ds.actual_level = 0;
            if (ds.arc_callback) ds.arc_callback(0);
            break;

        case DALI_CMD_UP:
            dali_fade_stop();
            if (ds.actual_level >= ds.max_level) break;
            if (ds.actual_level == 0) {
                ds.actual_level = ds.min_level;
                if (ds.arc_callback) ds.arc_callback(ds.min_level);
            }
            dali_fade_start_rate(ds.max_level, dali_fade_rate_ms[ds.fade_rate]);
            break;

        case DALI_CMD_DOWN:
            dali_fade_stop();
            if (ds.actual_level == 0 || ds.actual_level <= ds.min_level) break;
            dali_fade_start_rate(ds.min_level, dali_fade_rate_ms[ds.fade_rate]);
            break;

        case DALI_CMD_STEP_UP:
            dali_fade_stop();
            if (ds.actual_level == 0) ds.actual_level = ds.min_level;
            else if (ds.actual_level < ds.max_level) ds.actual_level++;
            else break;
            if (ds.arc_callback) ds.arc_callback(ds.actual_level);
            break;

        case DALI_CMD_STEP_DOWN:
            dali_fade_stop();
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
            ds.actual_level = ds.max_level;
            if (ds.arc_callback) ds.arc_callback(ds.max_level);
            break;

        case DALI_CMD_RECALL_MIN:
            dali_fade_stop();
            ds.actual_level = ds.min_level;
            if (ds.arc_callback) ds.arc_callback(ds.min_level);
            break;

        case DALI_CMD_STEP_DOWN_OFF:
            dali_fade_stop();
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
                for (uint8_t i = 0; i < 4; i++)
                    ds.colour_temp[i] = 254;
                ds.colour_tc = 0;
#endif
                ds.reset_state = 1;
                ds.power_cycle_seen = 0;
                if (ds.arc_callback) ds.arc_callback(ds.actual_level);
#if EVG_HAS_DT8
                if (ds.colour_callback)
                    ds.colour_callback((const uint8_t *)ds.colour_actual, EVG_NUM_COLOURS);
#endif
                nvm_mark_dirty();
                printf("RESET\n");
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
                printf("MAX=%d\n", ds.max_level);
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
                printf("MIN=%d\n", ds.min_level);
            }
            break;

        case DALI_CMD_DTR_AS_POWER_ON:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                ds.power_on_level = ds.dtr0;
                nvm_mark_dirty();
                ds.reset_state = 0;
                printf("PON=%d\n", ds.power_on_level);
            }
            break;

        case DALI_CMD_DTR_AS_SYS_FAIL:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                ds.sys_fail_level = ds.dtr0;
                nvm_mark_dirty();
                ds.reset_state = 0;
                printf("SFAIL=%d\n", ds.sys_fail_level);
            }
            break;

        case DALI_CMD_DTR_AS_FADE_TIME:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                ds.fade_time = ds.dtr0 & 0x0F;
                nvm_mark_dirty();
                ds.reset_state = 0;
                printf("FADE_TIME=%d\n", ds.fade_time);
            }
            break;

        case DALI_CMD_DTR_AS_FADE_RATE:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                uint8_t r = ds.dtr0 & 0x0F;
                if (r > 0) ds.fade_rate = r;
                nvm_mark_dirty();
                ds.reset_state = 0;
                printf("FADE_RATE=%d\n", ds.fade_rate);
            }
            break;

        case DALI_CMD_DTR_AS_SHORT_ADDR:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                if (ds.dtr0 == 0xFF) {
                    ds.short_address = 0xFF;
                } else {
                    ds.short_address = (ds.dtr0 >> 1) & 0x3F;
                }
                nvm_mark_dirty();
                ds.reset_state = 0;
                printf("SHORT_ADDR=%d\n", ds.short_address);
            }
            break;

        case DALI_CMD_DTR_AS_EXT_FADE:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                if (ds.dtr0 > 0x4F) {
                    ds.ext_fade_base = 0;
                    ds.ext_fade_mult = 0;
                } else {
                    ds.ext_fade_base = ds.dtr0 & 0x0F;
                    ds.ext_fade_mult = (ds.dtr0 >> 4) & 0x07;
                }
                nvm_mark_dirty();
                ds.reset_state = 0;
                printf("EXTFADE b=%d m=%d\n", ds.ext_fade_base, ds.ext_fade_mult);
            }
            break;

        case DALI_CMD_ENTER_BOOTLOADER:
            if (check_config_repeat(addr_byte, data_byte, now)) {
                printf("BOOTLOADER!\n");
                nvm_save();
                enter_bootloader();
            }
            break;

        default:
            /* Range-based commands */
            if (data_byte >= DALI_CMD_GO_TO_SCENE_BASE
                && data_byte <= DALI_CMD_GO_TO_SCENE_BASE + 15) {
                uint8_t scene = data_byte - DALI_CMD_GO_TO_SCENE_BASE;
                uint8_t slevel = ds.scene_level[scene];
                if (slevel == 0xFF) break;
                dali_fade_stop();
                slevel = clamp_level(slevel);
                uint32_t eff_fade_ms = dali_fade_get_effective_ms();
                if (slevel == 0 || eff_fade_ms == 0 || ds.actual_level == slevel) {
                    ds.actual_level = slevel;
                    if (ds.arc_callback) ds.arc_callback(ds.actual_level);
                } else {
                    dali_fade_start(slevel, eff_fade_ms);
                }
            } else if (data_byte >= DALI_CMD_STORE_SCENE_BASE
                       && data_byte <= DALI_CMD_STORE_SCENE_BASE + 15) {
                if (check_config_repeat(addr_byte, data_byte, now)) {
                    uint8_t scene = data_byte - DALI_CMD_STORE_SCENE_BASE;
                    ds.scene_level[scene] = ds.dtr0;
                    nvm_mark_dirty();
                    ds.reset_state = 0;
                    printf("SCENE%d=%d\n", scene, ds.dtr0);
                }
            } else if (data_byte >= DALI_CMD_REMOVE_SCENE_BASE
                       && data_byte <= DALI_CMD_REMOVE_SCENE_BASE + 15) {
                if (check_config_repeat(addr_byte, data_byte, now)) {
                    uint8_t scene = data_byte - DALI_CMD_REMOVE_SCENE_BASE;
                    ds.scene_level[scene] = 0xFF;
                    nvm_mark_dirty();
                    ds.reset_state = 0;
                    printf("RMSCENE%d\n", scene);
                }
            } else if (data_byte >= DALI_CMD_ADD_GROUP_BASE
                       && data_byte <= DALI_CMD_ADD_GROUP_BASE + 15) {
                if (check_config_repeat(addr_byte, data_byte, now)) {
                    uint8_t group = data_byte - DALI_CMD_ADD_GROUP_BASE;
                    ds.group_membership |= (1 << group);
                    nvm_mark_dirty();
                    ds.reset_state = 0;
                    printf("ADDGRP%d\n", group);
                }
            } else if (data_byte >= DALI_CMD_REMOVE_GROUP_BASE
                       && data_byte <= DALI_CMD_REMOVE_GROUP_BASE + 15) {
                if (check_config_repeat(addr_byte, data_byte, now)) {
                    uint8_t group = data_byte - DALI_CMD_REMOVE_GROUP_BASE;
                    ds.group_membership &= ~(1 << group);
                    nvm_mark_dirty();
                    ds.reset_state = 0;
                    printf("RMGRP%d\n", group);
                }
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

void dali_protocol_process(void) {
    dali_addressing_check_timeout();

    if (!dali_phy_frame_ready()) return;

    uint8_t raw[3];
    dali_phy_frame_bytes(raw);
    uint8_t bitlen = dali_phy_frame_bits();

    dali_frame_t frame = {
        .data      = ((uint32_t)raw[0] << 8) | (uint32_t)raw[1],
        .size      = bitlen,
        .flags     = DALI_FRAME_FLAG_FORWARD,
        .timestamp = millis(),
    };
    if (bitlen != 16) {
        frame.flags |= DALI_FRAME_FLAG_ERROR;
    }

    if (frame.size == 16 && !(frame.flags & DALI_FRAME_FLAG_ERROR)) {
        process_frame(&frame);
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
