# DALI EVG Test Cases (ch32fun Firmware)

## Hardware Setup

### Devices Under Test
| Device | MCU | Role | COM Port |
|--------|-----|------|----------|
| Raspberry Pi Pico | RP2040 (133 MHz) | DALI Master (bitbang) | COM9 |
| CH32V003F4P6 | RISC-V (48 MHz) | DALI Slave (control gear) | COM11 (debug serial) |

### Pin Connections
| Signal | Pico Pin | CH32V003 Pin | LA Channel | Notes |
|--------|----------|-------------|------------|-------|
| DALI Bus | GPIO17 (TX) | PC0 (RX) | D0 | Via DALI PHY transceiver |
| DALI Backward | GPIO16 (RX) | PC5 (TX) | D1 | Slave-to-master Manchester response via PHY |
| LED PWM 1 | — | PD2 (TIM1_CH1) | — | 20 kHz PWM (2400 steps) |
| LED PWM 2 | — | PA1 (TIM1_CH2) | — | 20 kHz PWM (2400 steps) |
| LED PWM 3 | — | PC3 (TIM1_CH3) | D7 | 20 kHz PWM (2400 steps) |
| LED PWM 4 | — | PC4 (TIM1_CH4) | — | 20 kHz PWM (2400 steps) |
| Serial Debug | USB CDC | PD5 (USART1_TX) | — | 115200 baud |

### Logic Analyzer
- Saleae clone (fx2lafw driver)
- Sample rate: 500 kHz (max recommended for this LA)
- Tool: sigrok-cli

### Flashing
- Pico: `pio run -t upload` via picotool (USB)
- CH32V003: `wlink.exe flash firmware.bin` via WCH-LinkE

---

## Firmware Info

| Property | Value |
|----------|-------|
| Framework | ch32fun (cnlohr/ch32fun) |
| Flash usage | 9,668 B (59.0%) |
| RAM usage | 136 B (6.6%) |
| PWM channels | 4 (configurable via `PWM_NUM_CHANNELS` in hardware.h) |
| Dimming curve | IEC 62386-102 §9.3 logarithmic (254-entry LUT, 508 bytes) |
| Features | Forward RX, Backward TX, PWM (1-4ch), Fade engine, DTR0/1/2, DT8 RGBW+Tc, Short address, Groups, Scenes, Min/Max/PowerOn levels, Memory bank 0 (read-only), TX collision detection (PHY) |

---

## TC-01: Forward Frame RX (Manchester Decode)

**Objective:** Verify the slave correctly decodes 16-bit DALI forward frames.

**Script:** `Debug_Helpers/ch32fun_test.ps1`

**Procedure:**
1. Send `broadcast <level>` from Pico master (COM9)
2. Read slave serial output (COM11) for decoded level
3. Repeat for levels 0, 128, 254

**Expected:**
- Slave prints `LVL=<level> PWM=<log_value>` for each command
- 100% decode rate (no missed or corrupted frames)

**Results:**
| Command | Frame (hex) | Slave Output | Status |
|---------|-------------|-------------|--------|
| `broadcast 0` | 0xFE00 | LVL=0 PWM=0 | PASS |
| `broadcast 128` | 0xFE80 | LVL=128 PWM=131 | PASS |
| `broadcast 254` | 0xFEFE | LVL=254 PWM=4095 | PASS |

**LA Verification (D0):** 32 edges per forward frame = correct for 16-bit Manchester encoding (1 start bit + 16 data bits).

---

## TC-02: Backward Frame TX (Manchester Encode)

**Objective:** Verify the slave responds with 8-bit backward frames to query commands.

**Script:** `Debug_Helpers/ch32fun_test.ps1`

**Procedure:**
1. Set a known level via `broadcast <level>`
2. Send query commands via `querybc <cmd>` from Pico master
3. Verify master receives correct response values

**Results:**
| Query | Cmd | Expected | Received | Status |
|-------|-----|----------|----------|--------|
| QUERY ACTUAL LEVEL | 160 | 128 (after broadcast 128) | 128 (0x80) | PASS |
| QUERY ACTUAL LEVEL | 160 | 100 (after broadcast 100) | 100 (0x64) | PASS |
| QUERY STATUS | 144 | Lamp on + missing short | 0x44 | PASS |
| QUERY GEAR PRESENT | 145 | YES (0xFF) | 255 (0xFF) | PASS |
| QUERY MAX LEVEL | 161 | 254 | 254 (0xFE) | PASS |
| QUERY MIN LEVEL | 162 | 1 | 1 (0x01) | PASS |
| QUERY POWER ON LEVEL | 163 | 254 | 254 (0xFE) | PASS |

**LA Verification (D1):** 18 edges per backward frame = correct for 8-bit Manchester (1 start + 8 data + stop transitions).

---

## TC-03: PWM Duty Cycle Accuracy (Logarithmic Dimming)

**Objective:** Verify PWM output follows IEC 62386-102 §9.3 logarithmic dimming curve.

**Script:** `Debug_Helpers/dali_logdim_test.ps1`

**Dimming Curve:** `PWM = round(10^((level-1)*3/253 - 1) / 100 * 4095)`

**Procedure:**
1. Send `broadcast <level>` for various levels
2. Read slave serial for PWM value (from log lookup table)
3. Capture PWM signal on LA channel D7, measure duty cycle

**Results:**
| DALI Level | Expected PWM | Measured PWM | Expected Duty | Measured Duty | Status |
|------------|-------------|-------------|---------------|---------------|--------|
| 0 | 0 | 0 | 0% | 0.0% | PASS |
| 1 | 4 | 4 | 0.1% | 0.1% | PASS |
| 64 | 23 | 23 | 0.6% | 0.6% | PASS |
| 128 | 131 | 131 | 3.2% | 3.2% | PASS |
| 170 | 391 | 391 | 9.5% | 9.5% | PASS |
| 191 | 733 | 733 | 17.9% | 17.9% | PASS |
| 220 | 1618 | 1618 | 39.5% | 39.5% | PASS |
| 240 | 2794 | 2794 | 68.2% | 68.2% | PASS |
| 254 | 4095 | 4095 | 100% | 99.6% | PASS |

**Note:** All 4 channels (PD2, PA1, PC3, PC4) output the same duty cycle.

---

## TC-04: Manchester Signal Integrity (LA)

**Objective:** Verify forward and backward frame signals are clean and independent.

**Script:** `Debug_Helpers/ch32fun_la_test.ps1`

**Procedure:**
1. Send `broadcast 128` (no backward frame expected)
2. Capture D0 + D1 simultaneously
3. Verify D0 has Manchester edges, D1 has none
4. Send `querybc 160` (backward frame expected)
5. Capture D0 + D1 simultaneously
6. Verify both channels have Manchester edges

**Results:**
| Test | D0 Edges | D1 Edges | Status |
|------|----------|----------|--------|
| Broadcast (no response) | 32 | 0 | PASS — signals independent |
| Query (with response) | 32 | 18 | PASS — backward frame present |

---

## TC-05: Timing Verification (IEC 62386-101)

**Objective:** Verify Manchester half-bit timing (Te) and backward frame settle time comply with IEC 62386-101.

**Script:** `Debug_Helpers/dali_timing_verify.ps1`

**IEC 62386-101 Requirements:**
- Te (half-bit period) = 416.67 µs ±10% (375–458 µs)
- Backward frame settle: 7–22 Te after forward frame ends

**Procedure:**
1. Send `querybc 160` from master
2. Capture D0 (forward) and D1 (backward) on LA
3. Measure edge-to-edge intervals for Te statistics
4. Measure gap between last D0 edge and first D1 edge for settle time

**Results:**
| Parameter | Spec | Measured | Status |
|-----------|------|----------|--------|
| Master TX Te (avg) | 416.67 µs ±10% | 417.4 µs | PASS |
| Master TX Te (range) | 375–458 µs | 416–420 µs | PASS |
| Slave TX Te (avg) | 416.67 µs ±10% | 415.2 µs | PASS |
| Slave TX Te (range) | 375–458 µs | 414–416 µs | PASS |
| Forward→Backward settle | 7–22 Te | 16 Te | PASS |
| Forward frame edges (D0) | 32 | 32 | PASS |
| Backward frame edges (D1) | 16–18 | 16 | PASS |

---

## TC-06: Short Address Assignment (DALI Addressing Protocol)

**Objective:** Verify the full DALI addressing protocol: INITIALISE, RANDOMISE, binary search (SEARCHADDR + COMPARE), PROGRAM SHORT ADDRESS, WITHDRAW, TERMINATE. Then verify the slave responds to commands addressed to its short address.

**Script:** `Debug_Helpers/ch32fun_assign_test.ps1`

**Procedure:**
1. Set a known level via `broadcast 128`
2. Run `assign 5` from master (performs full addressing sequence)
3. Verify INITIALISE accepted (slave prints `INIT ok`)
4. Verify RANDOMISE generated address (slave prints `RAND=XXXXXX`)
5. Verify binary search converges to exact random address (24 steps)
6. Verify PROGRAM SHORT succeeds (slave prints `PROG_SHORT=5`)
7. Verify query by short address returns correct level
8. Verify arc power by short address changes level
9. Verify broadcast still works after addressing

**Results:**
| Step | Expected | Actual | Status |
|------|----------|--------|--------|
| INITIALISE 0xFF (x2) | Accepted (config repeat) | `INIT ok` | PASS |
| RANDOMISE (x2) | Random addr generated | `RAND=C27054` | PASS |
| Binary search (24 steps) | Find exact random address | Found 0xC27054 | PASS |
| PROGRAM SHORT ADDRESS 5 | Address stored | `PROG_SHORT=5` | PASS |
| WITHDRAW | Slave withdrawn | `WITHDRAW` | PASS |
| Master verify (query addr 5) | Response with level | Level=128 | PASS |
| `query 5 160` after assign | 128 | 128 (0x80) | PASS |
| `arc 5 200` (short addr) | Level changes to 200 | `LVL=200 PWM=1618` | PASS |
| `query 5 160` after arc | 200 | 200 (0xC8) | PASS |
| `broadcast 50` after assign | Level changes to 50 | `LVL=50 PWM=16` | PASS |
| `querybc 160` | 50 | 50 (0x32) | PASS |
| `query 5 160` after broadcast | 50 | 50 (0x32) | PASS |
| TERMINATE | Init state disabled | `TERM` | PASS |

---

## TC-07: DALI Spec Compliance Fixes

**Objective:** Verify IEC 62386-102 spec compliance for edge cases found during code review.

**Procedure:**
1. Test MASK (0xFF) direct arc power — must NOT change level
2. Test QUERY MISSING SHORT ADDRESS with/without assigned address

**Results:**
| Test | IEC Requirement | Expected | Actual | Status |
|------|----------------|----------|--------|--------|
| `broadcast 255` (MASK) | No action (§9.5.1) | Level unchanged | Level unchanged | PASS |
| QUERY MISSING SHORT (no addr) | YES (0xFF) | 0xFF | 0xFF | PASS |
| QUERY MISSING SHORT (addr=5) | No response | Silence | Silence | PASS |

---

## TC-08: PWM Channel Configuration

**Objective:** Verify PWM output works on all 4 TIM1 channels when `PWM_NUM_CHANNELS = 4`.

**Procedure:**
1. Build with `PWM_NUM_CHANNELS = 4` (default)
2. Send `broadcast 128`
3. Measure PWM duty cycle on all 4 output pins

**Expected:** All channels output identical duty cycle.

| Channel | Pin | TIM1 Channel | Duty Cycle | Status |
|---------|-----|-------------|------------|--------|
| CH1 | PD2 | TIM1_CH1 | 3.2% | PASS |
| CH2 | PA1 | TIM1_CH2 | 3.2% | PASS |
| CH3 | PC3 | TIM1_CH3 | 3.2% | PASS |
| CH4 | PC4 | TIM1_CH4 | 3.2% | PASS |

---

## TC-09: Fade Engine (IEC 62386-102 §9.5)

**Objective:** Verify fadeTime-based arc power fading, fadeRate-based UP/DOWN, STEP UP/DOWN, and DTR0/config commands.

**Procedure:**
1. Set DTR0 to 4 via `raw A304` (DALI_SPECIAL_DTR, data=4)
2. Send STORE DTR AS FADE TIME twice: `raw FF2E` × 2 within 100 ms (cmd 46, config repeat)
3. Verify: `querybc 165` returns 0x47 (fadeTime=4, fadeRate=7)
4. Send `broadcast 254` — should fade from 0 to 254 over ~2 seconds (fadeTime=4 → 2000 ms)
5. Send `broadcast 0` — should go OFF instantly (OFF is always instant)
6. Test UP: `raw FF01` (cmd 1) — should fade up at fadeRate=7
7. Test DOWN: `raw FF02` (cmd 2) — should fade down at fadeRate=7
8. Test STEP UP: `raw FF03` (cmd 3) — should increase level by 1 instantly
9. Test STEP DOWN: `raw FF04` (cmd 4) — should decrease level by 1 instantly
10. Verify QUERY STATUS bit 4 (fadeRunning) is set during fade: `querybc 144`
11. Verify QUERY DTR0: `querybc 152` returns 4
12. Verify QUERY DEVICE TYPE: `querybc 153` returns 8 (colour control)
13. Verify QUERY PHYS MIN: `querybc 154` returns 1

**Results:**
| Test | Expected | Actual | Status |
|------|----------|--------|--------|
| STORE DTR AS FADE TIME | fadeTime=4 | — | — |
| QUERY FADE SPEEDS (165) | 0x47 | — | — |
| Arc 254 with fadeTime=4 | Fade over ~2 sec | — | — |
| OFF (cmd 0) | Instant to 0 | — | — |
| UP (cmd 1) | Fade up at fadeRate | — | — |
| DOWN (cmd 2) | Fade down at fadeRate | — | — |
| STEP UP (cmd 3) | Level +1 instant | — | — |
| STEP DOWN (cmd 4) | Level -1 instant | — | — |
| STATUS fadeRunning bit | Set during fade | — | — |
| QUERY DTR0 (152) | 4 | — | — |
| QUERY DEVICE TYPE (153) | 8 | — | — |
| QUERY PHYS MIN (154) | 1 | — | — |

---

## TC-10: DT8 Colour Control (IEC 62386-209)

**Objective:** Verify DT8 RGBW colour control: ENABLE_DT, SET_TEMP_RGB/WAF, ACTIVATE, colour temperature Tc, DT8 queries.

**Script:** `Debug_Helpers/ch32fun_dt8_test.ps1`

**DT8 Protocol Flow:**
```
1. Master: ENABLE_DT (0xC1), data=8    → slave stores enabled_device_type=8
2. Master: addr_byte, cmd (224-254)     → slave routes to DT8 handler
```

**Procedure:**
1. Verify QUERY DEVICE TYPE returns 8 (colour control)
2. Set DTR2=254, DTR1=0, DTR0=0 → ENABLE_DT 8 → SET_TEMP_RGB (235) → ENABLE_DT 8 → ACTIVATE (226)
   - Expected: CH1 (R) at full, CH2/CH3/CH4 at 0 — red only
3. Set DTR2=128 → ENABLE_DT 8 → SET_TEMP_WAF (236) → ENABLE_DT 8 → ACTIVATE (226)
   - Expected: CH4 (W) at half, CH1 (R) at full, CH2/CH3 at 0
4. ENABLE_DT 8 → QUERY_COLOUR_TYPE_FEATURES (249) → expect 0x06 (Tc + Primary)
5. ENABLE_DT 8 → QUERY_RGBWAF_CONTROL (251) → expect 0x0F (4 channels)
6. Set DTR0=64 → ENABLE_DT 8 → QUERY_COLOUR_VALUE (250) → expect 4 (num primaries)
7. Set DTR1=0, DTR0=154 (6500K mirek) → ENABLE_DT 8 → SET_TEMP_COLOUR_TEMPERATURE (231) → ENABLE_DT 8 → ACTIVATE → cool white
8. ENABLE_DT 8 → STEP_WARMER (233) → ENABLE_DT 8 → ACTIVATE → slightly warmer
9. DT6 backward compatibility: `broadcast 128` with colour set → all channels at half brightness with colour ratios preserved

**Results:**
| Test | Expected | Actual | Status |
|------|----------|--------|--------|
| QUERY DEVICE TYPE (153) | 8 | — | — |
| SET_TEMP_RGB (red only) + ACTIVATE | R=254, G=0, B=0 | — | — |
| SET_TEMP_WAF (add W=128) + ACTIVATE | R=254, W=128 | — | — |
| QUERY_COLOUR_TYPE_FEATURES (249) | 0x06 | — | — |
| QUERY_RGBWAF_CONTROL (251) | 0x0F | — | — |
| QUERY_COLOUR_VALUE (250, DTR0=64) | 4 | — | — |
| SET_TEMP_COLOUR_TEMP (6500K) + ACTIVATE | Cool white | — | — |
| STEP_WARMER + ACTIVATE | Slightly warmer | — | — |
| broadcast 128 with colour | Scaled RGBW | — | — |

---

## TC-11: DALI-1/2 Compliance (Scenes, Groups, Min/Max, RESET)

**Objective:** Verify scene storage/recall, group addressing, min/max level clamping, power-on level, RESET, and new queries.

**Procedure:**

### Part A: Min/Max Level Clamping
1. Set DTR0=100 via `raw A364` → STORE DTR AS MAX LEVEL (42) × 2: `raw FF2A` × 2
2. Verify: `querybc 161` → 100 (maxLevel)
3. Send `broadcast 200` → should clamp to 100
4. Send `broadcast 50` → should be 50 (within range)
5. Set DTR0=20 → STORE DTR AS MIN LEVEL (43) × 2
6. Verify: `querybc 162` → 20 (minLevel)
7. Send `broadcast 10` → should clamp to 20
8. STEP DOWN from level 20 → should go to 0 (OFF)
9. STEP UP from 0 → should go to minLevel (20)
10. UP from 0 → should start at minLevel (20), fade to maxLevel (100)
11. DOWN → should stop at minLevel (20), never go to 0

### Part B: Scenes
1. Set DTR0=128 → STORE DTR AS SCENE 0 (64) × 2: `raw FF40` × 2
2. Set DTR0=200 → STORE DTR AS SCENE 1 (65) × 2: `raw FF41` × 2
3. Verify: QUERY SCENE LEVEL 0 (176) → 128
4. Verify: QUERY SCENE LEVEL 1 (177) → 200
5. Verify: QUERY SCENE LEVEL 2 (178) → 0xFF (MASK, not set)
6. Send GO TO SCENE 0 (cmd 16): `raw FF10` → level should change to 128
7. Send GO TO SCENE 1 (cmd 17): `raw FF11` → level should change to 200
8. REMOVE FROM SCENE 0 (80) × 2 → QUERY SCENE 0 → 0xFF
9. GO TO SCENE 0 → no action (MASK)

### Part C: Group Addressing
1. Assign short address 5 first (via INITIALISE + binary search)
2. ADD TO GROUP 3 (99) × 2: `raw 0B63` × 2 (short addr 5, cmd 99)
3. Verify: QUERY GROUPS 0-7 (198) → 0x08 (bit 3 set)
4. Send group 3 broadcast arc power: address byte 0x86 (100 0011 0) → level should change
5. Send group 3 command OFF: address byte 0x87 (100 0011 1), data 0x00 → OFF
6. REMOVE FROM GROUP 3 (115) × 2 → QUERY GROUPS 0-7 → 0x00
7. Group 3 command → no response (not member)

### Part D: RESET
1. Modify several variables (maxLevel, minLevel, fadeTime, scenes, groups)
2. Send RESET (32) × 2: `raw FF20` × 2
3. Verify all defaults restored:
   - QUERY ACTUAL LEVEL → 254
   - QUERY MAX LEVEL → 254
   - QUERY MIN LEVEL → 1
   - QUERY POWER ON LEVEL → 254
   - QUERY FADE SPEEDS → 0x07 (fadeTime=0, fadeRate=7)
   - QUERY SCENE 0 → 0xFF
   - QUERY GROUPS 0-7 → 0x00
4. Verify short address NOT cleared (still assigned)

### Part E: Power-On Level & System Failure Level
1. Set DTR0=100 → STORE DTR AS POWER ON LEVEL (44) × 2
2. Verify: QUERY POWER ON LEVEL (163) → 100
3. Set DTR0=50 → STORE DTR AS SYSTEM FAILURE LEVEL (45) × 2
4. Verify: QUERY SYS FAIL LEVEL (164) → 50

### Part F: DTR1/DTR2 Queries
1. Set DTR1=42 via `raw C32A`, DTR2=99 via `raw C563`
2. QUERY DTR1 (155) → 42
3. QUERY DTR2 (156) → 99

**Results:**
| Test | Expected | Actual | Status |
|------|----------|--------|--------|
| MaxLevel clamp | Clamp to 100 | — | — |
| MinLevel clamp | Clamp to 20 | — | — |
| STEP DOWN from min | Goes to 0 | — | — |
| DOWN stops at min | Stops at 20 | — | — |
| Scene store + recall | Level changes | — | — |
| Scene remove + recall | No action | — | — |
| Group add + command | Responds | — | — |
| Group remove + command | No response | — | — |
| RESET defaults | All restored | — | — |
| RESET keeps short addr | Still assigned | — | — |
| Power-on level query | 100 | — | — |
| System failure query | 50 | — | — |
| DTR1/DTR2 queries | 42/99 | — | — |

---

## TC-12: Memory Bank 0 (READ MEMORY LOCATION)

**Objective:** Verify the read-only memory bank 0 returns the IEC 62386-102:2014 §4.3.10 layout via cmd 197 (READ MEMORY LOCATION) and that DTR1 post-increments correctly.

**Background:** Bank 0 identifies the gear: GTIN, FW/HW version, serial, 101/102/103 versions, logical-unit count. Master access is `DTR2 = bank, DTR1 = address, then cmd 197`. Each read post-increments DTR1 and mirrors the value into DTR0. Out-of-range / wrong-bank reads are silent (no backward frame).

**Procedure:**
1. Set DTR2 = 0 via `raw C500` (DALI_SPECIAL_DTR2)
2. Set DTR1 = 0 via `raw C300` (DALI_SPECIAL_DTR1)
3. Read 0x1B bytes via repeated `querybc 197` and verify each:

| DTR1 | Field | Expected (default build) |
|------|-------|--------------------------|
| 0x00 | Last accessible loc in bank 0 | 0x1A |
| 0x01 | Reserved | 0xFF |
| 0x02 | Last accessible bank | 0x00 |
| 0x03..0x08 | GTIN (6 B, MSB first) | 0x00 0x00 0x00 0x00 0x00 0x00 (placeholder) |
| 0x09 | FW version major | `DALI_FW_VERSION_MAJOR` (default 1) |
| 0x0A | FW version minor | `DALI_FW_VERSION_MINOR` (default 0) |
| 0x0B..0x12 | Identification number (8 B) | 0x00 × 8 (placeholder) |
| 0x13 | HW version major | `DALI_HW_VERSION_MAJOR` (default 1) |
| 0x14 | HW version minor | `DALI_HW_VERSION_MINOR` (default 0) |
| 0x15 | IEC 62386-101 version | 0x08 (DALI-2) |
| 0x16 | IEC 62386-102 version | 0x08 (DALI-2) |
| 0x17 | IEC 62386-103 version | 0xFF (not control device) |
| 0x18 | Logical control device units | 0xFF |
| 0x19 | Logical control gear units | 0x01 |
| 0x1A | Index of this gear unit | 0x00 |

4. After the last successful read DTR1 should be 0x1B. Verify via `querybc 155` (QUERY DTR1) → 0x1B.
5. Send `querybc 197` again at DTR1 = 0x1B → expect **silence** (out-of-range, no backward frame).
6. Set DTR2 = 1 via `raw C501`, DTR1 = 0 via `raw C300`, send `querybc 197` → expect **silence** (bank 1 not implemented).
7. Verify QUERY DTR0 (152) returns the last value successfully read from bank 0 (= 0x00 for the index byte at 0x1A).

**Results:**
| Test | Expected | Actual | Status |
|------|----------|--------|--------|
| Bank 0 byte 0x00 | 0x1A | — | — |
| Bank 0 byte 0x15 (101 version) | 0x08 | — | — |
| Bank 0 byte 0x16 (102 version) | 0x08 | — | — |
| Bank 0 byte 0x19 (gear units) | 0x01 | — | — |
| DTR1 post-increment after 27 reads | 0x1B | — | — |
| Read at DTR1 = 0x1B | Silent | — | — |
| Read at DTR2 = 1 | Silent | — | — |
| DTR0 mirrors last value | 0x00 | — | — |

**Notes:**
- GTIN and serial are zero placeholders. For production gear, override `DALI_GTIN_B0..5` and `DALI_SERIAL_B0..7` (in `hardware.h` or via `-D` flags) before flashing each unit.
- The byte layout in `dali_bank0.c` follows IEC 62386-102:2014 §4.3.10. A few field offsets shifted by one byte between -102:2009 and later editions; verify against the spec version your master expects.

---

## TC-13: TX Echo / Collision Detection (DALI PHY)

**Objective:** Verify `tx_collision_flag` is set when the bus level diverges from what the slave drove during a backward frame, and that the TX state machine releases the bus and aborts. Collision events are logged as `COLLISION` on the debug serial.

**Hardware requirement:** DALI PHY transceiver (open-drain, dominant-low bus) — the default firmware configuration. A second DALI device or test jig capable of pulling the bus active during the slave's backward frame is needed to provoke a collision.

**Procedure:**
1. Trigger a query that elicits a backward frame from the slave (e.g., `querybc 145` → expect 0xFF).
2. While the slave is mid-backward-frame, drive the bus dominant-low from a second device for ≥ 1 Te during a slot where the slave intends to drive idle (recessive).
3. Verify the slave's PC5 returns to idle (bus released) within 1 Te of the collision.
4. Read slave debug serial (COM11) — expect `COLLISION` printed once per event.
5. Re-issue the query — slave should respond normally (no latched error state).

**Results:**
| Test | Expected | Actual | Status |
|------|----------|--------|--------|
| Bus released on collision | PC5 → idle within 1 Te | — | — |
| `COLLISION` printed on debug serial | Once per event | — | — |
| Subsequent query succeeds | Normal backward frame | — | — |

---

## Test Scripts Index

| Script | Purpose |
|--------|---------|
| `ch32fun_test.ps1` | Forward + backward + PWM test |
| `ch32fun_la_test.ps1` | LA signal verification |
| `ch32fun_assign_test.ps1` | Short address assignment with binary search |
| `ch32fun_dt8_test.ps1` | DT8 colour control (RGBW, Tc, queries) |
| `dali_timing_verify.ps1` | IEC 62386-101 timing compliance (Te, settle time) |
| `dali_logdim_test.ps1` | IEC 62386-102 logarithmic dimming verification |
| `dali_test.ps1` | Basic DALI forward frame communication test |
| `dali_dim_test.ps1` | Step through brightness levels (visual LED check) |
| `dali_blink.ps1` | 1 Hz blink via DALI broadcast (continuous) |
| `dali_query_test.ps1` | Test all DALI query commands (backward frames) |
| `dali_assign_test.ps1` | Short address assignment (Arduino firmware) |
| `la_dali_test.ps1` | LA capture of DALI + LED signals |
| `d0d1_check.ps1` | Verify D0/D1 signal independence |
| `la_pwm_check.ps1` | PWM duty cycle verification at 5 levels |
| `read_pico.ps1` | Read Pico serial output (COM9) |
| `read_ch32.ps1` | Read CH32 serial output (COM11) |
| `reset_and_read.ps1` | Reset CH32 via wlink + read serial |

---

## Known Issues

1. **First frame after reset** — Occasionally the very first DALI frame after power-up decodes incorrectly (e.g., 0x7E80 instead of 0xFE80). Subsequent frames are 100% correct.
