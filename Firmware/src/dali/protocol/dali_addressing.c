/*
    dali_addressing.c - DALI addressing protocol (IEC 62386-102 §9.6)

    Handles all special commands: INITIALISE, RANDOMISE, binary search
    (COMPARE, SEARCHADDR, WITHDRAW), PROGRAM SHORT ADDRESS, VERIFY SHORT,
    QUERY SHORT, TERMINATE, DTR0/1/2, and ENABLE DEVICE TYPE.
*/

#include "ch32fun.h"
#include <stdio.h>
#include "dali_addressing.h"
#include "../dali_state.h"
#include "../dali_physical.h"
#include "../phy/dali_phy.h"
#include "../nvm/dali_nvm.h"

/* millis() provided by main.c */
extern uint32_t millis(void);

/* ── Addressing state (module-private) ───────────────────────────── */
typedef enum {
    INIT_DISABLED,
    INIT_ENABLED,
    INIT_WITHDRAWN
} init_state_t;

static volatile init_state_t init_state = INIT_DISABLED;
static volatile uint32_t    init_start_time = 0;
static volatile uint8_t     random_h = 0, random_m = 0, random_l = 0;
static volatile uint8_t     search_h = 0xFF, search_m = 0xFF, search_l = 0xFF;

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
 *  PUBLIC API                                                         *
 * ================================================================== */

void dali_addressing_process_special(uint8_t addr_byte, uint8_t data_byte) {
    uint32_t now = millis();

    switch (addr_byte) {
    case DALI_SPECIAL_TERMINATE:
        init_state = INIT_DISABLED;
        printf("TERM\n");
        break;

    case DALI_SPECIAL_DTR:
        ds.dtr0 = data_byte;
        break;

    case DALI_SPECIAL_INITIALISE:
        if (check_config_repeat(addr_byte, data_byte, now)) {
            uint8_t addressed = 0;
            if (data_byte == 0xFF) {
                addressed = 1;
            } else if (data_byte == 0x00) {
                addressed = (ds.short_address == 0xFF);
            } else if (data_byte & 1) {
                addressed = (((data_byte >> 1) & 0x3F) == ds.short_address);
            }
            if (addressed) {
                init_state = INIT_ENABLED;
                init_start_time = now;
                printf("INIT ok\n");
            }
        }
        break;

    case DALI_SPECIAL_RANDOMISE:
        if (init_state != INIT_ENABLED) break;
        if (check_config_repeat(addr_byte, data_byte, now)) {
            uint32_t seed = SysTick->CNT;
            seed ^= (uint32_t)ds.short_address << 16;
            seed ^= (uint32_t)ds.actual_level << 8;
            seed *= 1103515245UL;
            seed += 12345;
            random_h = (seed >> 16) & 0xFF;
            random_m = (seed >> 8) & 0xFF;
            random_l = seed & 0xFF;
            printf("RAND=%02X%02X%02X\n", random_h, random_m, random_l);
        }
        break;

    case DALI_SPECIAL_COMPARE:
        if (init_state != INIT_ENABLED) break;
        {
            uint32_t random = ((uint32_t)random_h << 16) | ((uint32_t)random_m << 8) | random_l;
            uint32_t search = ((uint32_t)search_h << 16) | ((uint32_t)search_m << 8) | search_l;
            if (random <= search) {
                dali_phy_send_backward(0xFF);
            }
        }
        break;

    case DALI_SPECIAL_WITHDRAW:
        if (init_state != INIT_ENABLED) break;
        {
            uint32_t random = ((uint32_t)random_h << 16) | ((uint32_t)random_m << 8) | random_l;
            uint32_t search = ((uint32_t)search_h << 16) | ((uint32_t)search_m << 8) | search_l;
            if (random == search) {
                init_state = INIT_WITHDRAWN;
                printf("WITHDRAW\n");
            }
        }
        break;

    case DALI_SPECIAL_SEARCHADDRH:
        search_h = data_byte;
        break;

    case DALI_SPECIAL_SEARCHADDRM:
        search_m = data_byte;
        break;

    case DALI_SPECIAL_SEARCHADDRL:
        search_l = data_byte;
        break;

    case DALI_SPECIAL_PROGRAM_SHORT:
        if (init_state != INIT_ENABLED) break;
        {
            uint32_t random = ((uint32_t)random_h << 16) | ((uint32_t)random_m << 8) | random_l;
            uint32_t search = ((uint32_t)search_h << 16) | ((uint32_t)search_m << 8) | search_l;
            if (random == search) {
                if (data_byte == 0xFF) {
                    ds.short_address = 0xFF;
                } else {
                    ds.short_address = (data_byte >> 1) & 0x3F;
                }
                nvm_mark_dirty();
                printf("PROG_SHORT=%d\n", ds.short_address);
            } else {
                printf("PROG_SHORT: random!=search R=%06lX S=%06lX\n",
                       (unsigned long)random, (unsigned long)search);
            }
        }
        break;

    case DALI_SPECIAL_VERIFY_SHORT:
        if (init_state != INIT_ENABLED) break;
        {
            uint8_t addr = (data_byte >> 1) & 0x3F;
            if (addr == ds.short_address) {
                dali_phy_send_backward(0xFF);
            }
        }
        break;

    case DALI_SPECIAL_QUERY_SHORT:
        if (init_state != INIT_ENABLED) break;
        {
            uint32_t random = ((uint32_t)random_h << 16) | ((uint32_t)random_m << 8) | random_l;
            uint32_t search = ((uint32_t)search_h << 16) | ((uint32_t)search_m << 8) | search_l;
            if (random == search) {
                if (ds.short_address == 0xFF) {
                    dali_phy_send_backward(0xFF);
                } else {
                    dali_phy_send_backward((ds.short_address << 1) | 1);
                }
            }
        }
        break;

    case DALI_SPECIAL_DTR1:
        ds.dtr1 = data_byte;
        break;

    case DALI_SPECIAL_DTR2:
        ds.dtr2 = data_byte;
        break;

    case DALI_SPECIAL_ENABLE_DT:
        ds.enabled_device_type = data_byte;
        break;

    default:
        break;
    }
}

/* IEC 62386-102 §9.6.3: The control gear shall leave the initialisation
 * state no later than 15 minutes after the last INITIALISE command.
 * This prevents the device from being stuck in addressing mode forever
 * if the master crashes or never sends TERMINATE. Called every main loop
 * iteration — returns immediately when not in init state (cheap check). */
void dali_addressing_check_timeout(void) {
    if (init_state != INIT_DISABLED) {
        if (millis() - init_start_time > DALI_INIT_TIMEOUT_MS) {
            init_state = INIT_DISABLED;
        }
    }
}

uint8_t dali_addressing_in_init(void) {
    return (init_state == INIT_ENABLED);
}

uint8_t dali_addressing_random_h(void) { return random_h; }
uint8_t dali_addressing_random_m(void) { return random_m; }
uint8_t dali_addressing_random_l(void) { return random_l; }
