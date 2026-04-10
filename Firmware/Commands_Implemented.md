# DALI Commands Implemented

DALI-2 control gear firmware for CH32V003F4P6 (ch32fun framework).
Implements IEC 62386-101, IEC 62386-102, and IEC 62386-209 (DT8).

## IEC 62386-101 — Physical Layer

| Feature | Status | Notes |
|---------|--------|-------|
| Manchester encoding 1200 baud | Done | Te = 417 us, +/-30% acceptance window |
| Forward frame RX (16-bit) | Done | EXTI both-edge + TIM2 timestamping |
| Backward frame TX (8-bit) | Done | TIM2 output compare, settle 7-22 Te |
| Noise filter | Done | Edges < 200 us ignored |
| Idle timeout (frame complete) | Done | 5 Te, TIM2 CH4 output compare |
| PHY mode (default) | Done | Normal polarity for DALI transceiver (TX: HIGH=active, RX: HIGH=active) |
| DALI_NO_PHY mode | Done | Direct GPIO, inverted polarity (TX: LOW=active, RX: LOW=active). Optional, disabled by default. |
| Bus collision detection (TX echo check) | Done | TX ISR samples bus each Te via PHY readback, aborts on mismatch within 1 Te. Collision logged via `printf("COLLISION")` and flag readable via `dali_tx_consume_collision()`. Requires PHY (open-drain bus). |
| Structured frame type | Done | `dali_frame_t { data, size, flags, timestamp }` with FORWARD / BACKWARD / ERROR / COLLISION / ECHO flags (`dali_frame.h`) |

## IEC 62386-102 — Immediate Action Commands (0-15)

| Cmd | Name | Status | Notes |
|-----|------|--------|-------|
| 0 | OFF | Done | Instant to level 0, cancels fade |
| 1 | UP | Done | Fade up at fadeRate to maxLevel |
| 2 | DOWN | Done | Fade down at fadeRate to minLevel |
| 3 | STEP UP | Done | +1 level instant, 0 -> minLevel |
| 4 | STEP DOWN | Done | -1 level instant, minLevel -> 0 |
| 5 | RECALL MAX LEVEL | Done | Instant to maxLevel |
| 6 | RECALL MIN LEVEL | Done | Instant to minLevel |
| 7 | STEP DOWN AND OFF | Done | Step down, OFF at minLevel |
| 8 | ON AND STEP UP | Done | If OFF -> minLevel, else step up |
| 9 | ENABLE DAPC SEQUENCE | -- | Not implemented (rarely used) |
| 10-15 | Reserved | -- | |

## IEC 62386-102 — Scene Commands (16-31)

| Cmd | Name | Status | Notes |
|-----|------|--------|-------|
| 16-31 | GO TO SCENE 0-15 | Done | Recall with fadeTime, MASK ignored |

## IEC 62386-102 — Configuration Commands (32-128)

All config commands require config repeat (2x within 100 ms).

| Cmd | Name | Status | Notes |
|-----|------|--------|-------|
| 32 | RESET | Done | Restore Table 22 defaults, preserves short addr |
| 33 | STORE ACTUAL LEVEL IN DTR0 | Done | dtr0 = actualLevel |
| 34-41 | Reserved | -- | |
| 42 | STORE DTR AS MAX LEVEL | Done | Persisted to flash |
| 43 | STORE DTR AS MIN LEVEL | Done | Clamped to [1, maxLevel] (dimming) or forced 254 (ONOFF) |
| 44 | STORE DTR AS POWER ON LEVEL | Done | Applied at boot |
| 45 | STORE DTR AS SYSTEM FAILURE LEVEL | Done | Persisted to flash |
| 46 | STORE DTR AS FADE TIME | Done | Lower 4 bits, 0-15 |
| 47 | STORE DTR AS FADE RATE | Done | Lower 4 bits, 1-15 (0 reserved) |
| 48 | STORE DTR AS SHORT ADDRESS | Done | DTR0 = (addr<<1)\|1, or 0xFF to delete |
| 49-63 | Reserved | -- | |
| 64-79 | STORE DTR AS SCENE 0-15 | Done | Persisted to flash |
| 80-95 | REMOVE FROM SCENE 0-15 | Done | Sets scene to MASK (0xFF) |
| 96-111 | ADD TO GROUP 0-15 | Done | 16-bit membership bitmask |
| 112-127 | REMOVE FROM GROUP 0-15 | Done | |
| 128 | STORE DTR AS EXTENDED FADE TIME | Done | Bits [3:0]=base, [6:4]=mult, >0x4F resets to 0. Persisted. |

## IEC 62386-102 — Query Commands (144-199)

| Cmd | Name | Status | Response |
|-----|------|--------|----------|
| 144 | QUERY STATUS | Done | Status byte (see below) |
| 145 | QUERY CONTROL GEAR PRESENT | Done | Always YES (0xFF) |
| 146 | QUERY LAMP FAILURE | Done | No response (no HW monitoring) |
| 147 | QUERY LAMP POWER ON | Done | YES if actualLevel > 0 |
| 148 | QUERY LIMIT ERROR | Done | No response (no limit errors) |
| 149 | QUERY RESET STATE | Done | YES if all vars at defaults |
| 150 | QUERY MISSING SHORT ADDRESS | Done | YES if addr = 0xFF |
| 151 | QUERY VERSION NUMBER | Done | Returns 1 (DALI-1) |
| 152 | QUERY CONTENT DTR0 | Done | Returns DTR0 |
| 153 | QUERY DEVICE TYPE | Done | Returns 6 (DT6) or 8 (DT8), depending on EVG mode |
| 154 | QUERY PHYSICAL MINIMUM | Done | Returns 1 (dimming modes) or 254 (ONOFF mode) |
| 155 | QUERY CONTENT DTR1 | Done | Returns DTR1 |
| 156 | QUERY CONTENT DTR2 | Done | Returns DTR2 |
| 157 | QUERY POWER FAILURE | -- | DALI-2 only |
| 158-159 | Reserved | -- | |
| 160 | QUERY ACTUAL LEVEL | Done | 0-254 |
| 161 | QUERY MAX LEVEL | Done | |
| 162 | QUERY MIN LEVEL | Done | |
| 163 | QUERY POWER ON LEVEL | Done | |
| 164 | QUERY SYSTEM FAILURE LEVEL | Done | |
| 165 | QUERY FADE TIME / FADE RATE | Done | (fadeTime<<4) \| fadeRate |
| 166 | QUERY POSSIBLE OPERATING MODES | -- | DALI-2 diagnostic |
| 167 | QUERY FEATURES | -- | DALI-2 diagnostic |
| 168 | QUERY FAILURE STATUS | -- | DALI-2 diagnostic |
| 169 | QUERY SHORT CIRCUIT | -- | DALI-2 diagnostic |
| 170 | QUERY OPEN CIRCUIT | -- | DALI-2 diagnostic |
| 171 | QUERY LOAD DECREASE | -- | DALI-2 diagnostic |
| 172 | QUERY LOAD INCREASE | -- | DALI-2 diagnostic |
| 173 | QUERY INSULATION FAULT | -- | DALI-2 diagnostic |
| 174 | QUERY OUTPUT POWER ON | -- | DALI-2 diagnostic |
| 175 | QUERY THERMAL SHUTDOWN | -- | DALI-2 diagnostic |
| 176-191 | QUERY SCENE LEVEL 0-15 | Done | 0-254, 0xFF = MASK |
| 192-193 | Reserved | -- | |
| 194 | QUERY RANDOM ADDRESS H | Done | |
| 195 | QUERY RANDOM ADDRESS M | Done | |
| 196 | QUERY RANDOM ADDRESS L | Done | |
| 197 | READ MEMORY LOCATION | Done | Reads bank0[DTR1] when DTR2=0; mirrors value into DTR0; post-increments DTR1. Out-of-range / wrong bank: silent. |
| 198 | QUERY GROUPS 0-7 | Done | Bitmask |
| 199 | QUERY GROUPS 8-15 | Done | Bitmask |

### QUERY STATUS Byte (cmd 144)

| Bit | Name | Status | Description |
|-----|------|--------|-------------|
| 0 | controlGearFailure | Always 0 | No HW fault monitoring |
| 1 | lampFailure | Always 0 | No HW lamp monitoring |
| 2 | lampOn | Done | 1 if actualLevel > 0 |
| 3 | limitError | Always 0 | Not tracked |
| 4 | fadeRunning | Done | 1 if fade in progress |
| 5 | resetState | Done | 1 after RESET, cleared on config change |
| 6 | missingShortAddress | Done | 1 if short_address = 0xFF |
| 7 | powerCycleSeen | Done | 1 after power-on, cleared by RESET |

## IEC 62386-102 — Special Commands

| Addr Byte | Name | Status | Notes |
|-----------|------|--------|-------|
| 0xA1 | TERMINATE | Done | Leave initialisation state |
| 0xA3 | DATA TRANSFER REGISTER (DTR0) | Done | Set DTR0 |
| 0xA5 | INITIALISE | Done | Config repeat, 15-min timeout |
| 0xA7 | RANDOMISE | Done | Config repeat, LCG from SysTick |
| 0xA9 | COMPARE | Done | random <= search -> YES |
| 0xAB | WITHDRAW | Done | random == search -> withdraw |
| 0xB1 | SEARCHADDRH | Done | Set search address high byte |
| 0xB3 | SEARCHADDRM | Done | Set search address mid byte |
| 0xB5 | SEARCHADDRL | Done | Set search address low byte |
| 0xB7 | PROGRAM SHORT ADDRESS | Done | Assign addr during init |
| 0xB9 | VERIFY SHORT ADDRESS | Done | YES if addr matches |
| 0xBB | QUERY SHORT ADDRESS | Done | Returns addr if random==search |
| 0xC1 | ENABLE DEVICE TYPE | Done | Consumed by next command |
| 0xC3 | DTR1 | Done | Set DTR1 |
| 0xC5 | DTR2 | Done | Set DTR2 |

## IEC 62386-102 — Addressing

| Mode | Status | Notes |
|------|--------|-------|
| Short address (0AAAAAA S) | Done | 0-63 |
| Group address (100GGGG S) | Done | 16 groups, bitmask |
| Broadcast (1111111 S) | Done | 0xFE arc, 0xFF command |
| MASK handling (0xFF) | Done | No action on arc power 0xFF |

## IEC 62386-102 — Protocol Features

| Feature | Status | Notes |
|---------|--------|-------|
| Config repeat (100 ms window) | Done | Commands 32-128, INITIALISE, RANDOMISE |
| 15-minute initialisation timeout | Done | |
| Min/max level clamping | Done | All arc power paths |
| Logarithmic dimming curve | Done | IEC 62386-102 Table 16, 254-entry LUT |
| Fade engine (fadeTime) | Done | Arc power + scenes |
| Fade engine (fadeRate) | Done | UP/DOWN commands |
| Power-on level | Done | Applied from NVM at boot |
| Flash persistence (NVM) | Done | All config survives power cycle |
| DT8 colour persistence | Done | RGBW + Tc persisted in NVM |
| Deferred NVM write | Done | 5-second dirty timer |

## IEC 62386-209 — DT8 Colour Control

| Cmd | Name | Status | Notes |
|-----|------|--------|-------|
| 226 | ACTIVATE | Done | Commit temp colour -> actual, persisted |
| 231 | SET TEMPORARY COLOUR TEMPERATURE | Done | Mirek from DTR1:DTR0 |
| 232 | STEP COOLER | Done | -10 mirek (toward 6500K) |
| 233 | STEP WARMER | Done | +10 mirek (toward 2700K) |
| 235 | SET TEMPORARY RGB LEVEL | Done | R=DTR2, G=DTR1, B=DTR0 |
| 236 | SET TEMPORARY WAF LEVEL | Done | W=DTR2 (A, F ignored) |
| 238 | COPY REPORT TO TEMPORARY | Done | Copy actual -> temp |

### DT8 Query Commands

| Cmd | Name | Status | Response |
|-----|------|--------|----------|
| 247 | QUERY GEAR FEATURES | Done | 0x00 (no special features) |
| 248 | QUERY COLOUR STATUS | Done | Tc/RGBWAF active bits |
| 249 | QUERY COLOUR TYPE FEATURES | Done | Tc + Primary supported |
| 250 | QUERY COLOUR VALUE | Done | DTR0-dependent |
| 251 | QUERY RGBWAF CONTROL | Done | Active channel bitmask |
| 252 | QUERY ASSIGNED COLOUR | Done | 0xFF |

### DT8 Features

| Feature | Status | Notes |
|---------|--------|-------|
| RGBW output (4 channels) | Done | TIM1 CH1-CH4 PWM |
| Tc -> RGBW conversion | Done | Linear interpolation 2700K-6500K |
| CIE xy chromaticity | -- | Requires spectral calibration |
| Tc limits (STORE/QUERY) | -- | Not implemented |
| Auto-calibration | -- | Not implemented |
| DT6 backward compatibility | Done | All channels equal when no DT8 colour set |
| Colour persistence in NVM | Done | RGBW + Tc survive power cycle |

## IEC 62386-102 — Memory Banks

| Bank | Access | Status | Notes |
|------|--------|--------|-------|
| 0 | Read-only | Done | Gear identification (`dali_bank0.c`): GTIN, FW/HW versions, serial, 101/102/103 versions, logical-unit count. 27 bytes (last addr 0x1A). GTIN/serial are zero placeholders pending provisioning. |
| 1 | Read/write | -- | Luminaire data (rated power, colour info). Not implemented. |
| 2+ | Vendor-specific | -- | Not implemented. |

Read access via cmd 197 (READ MEMORY LOCATION) with DTR2 = bank, DTR1 = address.
Bank-write commands (ENABLE WRITE MEMORY, WRITE MEMORY LOCATION 0xC7/0xC9) are not implemented.

## Not Implemented

| Feature | Reason |
|---------|--------|
| ENABLE DAPC SEQUENCE (cmd 9) | Rarely used, complex timing |
| DALI-2 diagnostic queries (166-175) | Require HW monitoring circuitry |
| QUERY POWER FAILURE (157) | DALI-2, no HW monitoring |
| CIE xy chromaticity | Requires spectral calibration per LED |
| Tc temperature limits | Not implemented |
| Bus collision detection on direct GPIO (`DALI_NO_PHY`) | TX echo check cannot detect collisions on push-pull GPIO; use PHY mode (default) |
| Memory bank 1 + write access | Requires r/w storage strategy and lock/unlock state machine |
| Extended fade time | Done — used when fadeTime=0 (up to ~16 min fades) |

## Resource Usage

| Resource | RGBW | ONOFF |
|----------|------|-------|
| Flash | 9,668 B (59.0% of 16 KB) | — |
| RAM | 136 B (6.6% of 2 KB) | — |
| NVM page | 64 B at 0x08003FC0 (last flash page) |
| TIM1 | PWM (4 channels, 20 kHz) |
| TIM2 | DALI timing (1 MHz free-running) |
| EXTI0 | DALI RX (PC0, both edges) |
| SysTick | millis() (1 ms tick) |
| USART1 | Debug printf (PD5, 115200 baud) |
