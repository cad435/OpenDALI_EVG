/*
    dali_state.h - Shared DALI device state (all persistent + runtime variables)

    This struct is the single source of truth for all DALI operating
    parameters. It is instantiated once in dali_protocol.c and accessed
    by all sub-modules (dali_protocol, dali_addressing, dali_dt8,
    dali_fade) via the extern pointer.

    All members are volatile because they may be read/written from both
    ISR and main-loop contexts. On RISC-V CH32V003, single-byte and
    aligned-word accesses are atomic — no additional synchronisation needed.
*/
#ifndef _DALI_STATE_H
#define _DALI_STATE_H

#include <stdint.h>
#include "../config/hardware.h"
#include "dali_slave.h"         /* callback typedefs */

typedef struct {
    /* ── Arc power ──────────────────────────────────────────────── */
    volatile uint8_t     actual_level;       /* Current arc power level (0–254) */
    volatile uint8_t     max_level;          /* Maximum allowed arc level */
    volatile uint8_t     min_level;          /* Minimum allowed arc level (>0) */
    volatile uint8_t     power_on_level;     /* Level applied at power-on */
    volatile uint8_t     sys_fail_level;     /* Level on system failure */

    /* ── Fade parameters ────────────────────────────────────────── */
    volatile uint8_t     fade_time;          /* 0–15, 0 = instant */
    volatile uint8_t     fade_rate;          /* 1–15, default 7 */
    volatile uint8_t     ext_fade_base;      /* DALI-2: lower 4 bits of DTR0 */
    volatile uint8_t     ext_fade_mult;      /* DALI-2: bits 6:4 of DTR0 */

    /* ── Addressing ─────────────────────────────────────────────── */
    volatile uint8_t     short_address;      /* 0–63 or 0xFF (unassigned) */
    volatile uint16_t    group_membership;   /* Bit N = member of group N */
    volatile uint8_t     scene_level[16];    /* 0xFF = MASK (not in scene) */

    /* ── Data transfer registers ────────────────────────────────── */
    volatile uint8_t     dtr0;
    volatile uint8_t     dtr1;
    volatile uint8_t     dtr2;
    volatile uint8_t     enabled_device_type; /* ENABLE_DT state, consumed on next cmd */

    /* ── DT8 colour state ───────────────────────────────────────── */
    volatile uint8_t     colour_actual[4];   /* Active RGBW (default 254) */
#if EVG_HAS_DT8
    volatile uint8_t     colour_temp[4];     /* Staging RGBW */
    volatile uint16_t    colour_tc;          /* Mirek, 0 = not set */
#endif

    /* ── Status flags ───────────────────────────────────────────── */
    volatile uint8_t     reset_state;        /* 1 after boot/RESET */
    volatile uint8_t     power_cycle_seen;   /* 1 after power-on */

    /* ── Callbacks ──────────────────────────────────────────────── */
    dali_arc_callback_t    arc_callback;
    dali_colour_callback_t colour_callback;
} dali_device_state_t;

/* Single global instance — defined in dali_protocol.c */
extern dali_device_state_t ds;

/* ── Shared helpers ─────────────────────────────────────────────── */

/* Enforce min/max level constraints (0 = OFF always valid) */
static inline uint8_t clamp_level(uint8_t level) {
    if (level == 0) return 0;
    if (level < ds.min_level) return ds.min_level;
    if (level > ds.max_level) return ds.max_level;
    return level;
}

/* Check if forward frame address byte targets this device */
static inline uint8_t is_addressed_to_me(uint8_t addr_byte) {
    uint8_t Y = (addr_byte >> 7) & 1;
    if (Y == 0) {
        uint8_t addr = (addr_byte >> 1) & 0x3F;
        return (addr == ds.short_address);
    }
    if ((addr_byte & 0xFE) == 0xFE) return 1;  /* Broadcast */
    if ((addr_byte & 0xE0) == 0x80) {           /* Group */
        uint8_t group = (addr_byte >> 1) & 0x0F;
        return (ds.group_membership & (1 << group)) ? 1 : 0;
    }
    return 0;
}

#endif /* _DALI_STATE_H */
