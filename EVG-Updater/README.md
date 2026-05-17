# DALI-bus Firmware Updater

<p align="center">
<img width="583" height="753" alt="image" src="https://github.com/user-attachments/assets/fdf3689b-21b4-4ad6-ae88-bf06fd46421c" />
</p>

C# WinForms application for updating and inspecting OpenDALI EVG devices via the DALI bus, using an OpenKNX GW-REG1-Dali gateway.

Implements the IEC 62386-105 firmware update protocol over 32-bit DALI forward frames, plus a read-only bus scan that probes shorts 0..63 and reads bank 0 identity.

> Trademark notice — see [root README](../README.md): *DALI*, *DALI-2* etc. are DiiA trademarks; this project is an independent IEC 62386 implementation, not DiiA-certified. Educational purposes only

## Requirements

- .NET 8.0 SDK
- OpenKNX GW-REG1-Dali gateway on the network
- Target device running OpenDALI EVG firmware with IEC 62386-105 bootloader in boot area

## Build

```bash
dotnet build EVG_Updater.slnx
```

## Usage

```
EVG_Updater <command> [options]
```

Commands:

| Command | Purpose |
|---|---|
| `flash <firmware.bin>` | Flash a firmware image to an EVG via DALI bus |
| `scan` | Probe the DALI bus and list discovered gear |
| *(no args)* | Launch the graphical interface |

Run `EVG_Updater <command> --help` for per-command details.

### GUI Mode

Run without arguments to launch the graphical interface:

```bash
EVG_Updater.exe
```

The GUI provides:
- Gateway IP configuration (default: 192.168.178.131)
- Bus scan with sortable device grid (Short / Random / GTIN / Mode / DT / FW / HW / Ours)
- Firmware .bin file selection
- DALI short address (0-63)
- GTIN and EVG Mode ID for Block 0 validation
- Progress bar and scrollable log output

### `flash` — Firmware Update

```bash
EVG_Updater.exe flash firmware.bin [options]
```

Options:

| Option | Default | Description |
|--------|---------|-------------|
| `--ip <address>` | 192.168.178.131 | Gateway IP address |
| `--addr <0-63>` | 0 | DALI short address of target device |
| `--gtin <hex>` | 3452334E0CAD | 6-byte GTIN in hex (must match device EEPROM) |
| `--mode <1-8>` | 5 (RGBW) | EVG mode ID (must match device EEPROM) |
| `--help`, `-h` | | Show usage help |

Exit codes:
- `0` — Success
- `1` — Argument or connection error
- `2` — Firmware update failed

Examples:

```bash
# Update device at address 0 with default settings
EVG_Updater.exe flash firmware.bin

# Update device at address 5, RGB mode
EVG_Updater.exe flash firmware.bin --addr 5 --mode 4

# Use a different gateway
EVG_Updater.exe flash firmware.bin --ip 10.0.0.50

# Build and flash in one command (from Firmware/ directory)
pio run && ../EVG-Updater/EVG_Updater.exe flash .pio/build/genericCH32V003F4P6/firmware.bin
```

### `scan` — Bus Discovery

Probes every short address 0..63 with `QUERY GEAR PRESENT`, then for each
responding gear reads bank 0 (GTIN, FW/HW versions, EVG mode via serial
field) and the 24-bit random address. Read-only — never modifies addressing
or any persistent state.

```bash
EVG_Updater.exe scan [options]
```

Options:

| Option | Default | Description |
|--------|---------|-------------|
| `--ip <address>` | 192.168.178.131 | Gateway IP address |
| `--ours-gtin <hex>` | 3452334E0CAD | GTIN to flag as "ours" in the Ours column |
| `--quiet`, `-q` | | Suppress progress log, print only the result table |
| `--help`, `-h` | | Show usage help |

Output mirrors the GUI grid columns: **Short, Random, GTIN, Mode, DT, FW, HW, Ours**.

Example (quiet mode for scripting):

```bash
$ EVG_Updater.exe scan --quiet
Short  Random    GTIN          Mode  DT     FW   HW   Ours
-----  --------  ------------  ----  -----  ---  ---  ----
0      0xCAF8CB  3452334E0CAD  RGBW  DT8    0.2  0.1  yes
1      0x93D441  3452334E0CAD  RGBW  DT8    0.2  0.1  yes
2      0xFE7CCA  —             ?     multi  —    —    no
```

Underlying DALI sequence (IEC 62386-102 Ed 2.0 §11.x cmd 197): `QUERY GEAR PRESENT`
on each short, then once globally `DTR1 ← 0` (bank 0) and `DTR0 ← 0x03` (GTIN start)
via the special commands `0xC3` / `0xA3`, then per gear 18× `READ MEMORY LOCATION`
to walk bank 0 offsets 0x03..0x14 (post-increment of DTR0 is gear-local, so back-to-back
reads on the same gear walk the bank without re-setting DTR0), plus `QUERY DEVICE TYPE`
and `QUERY RANDOM ADDRESS (H/M/L)` per gear.

Devices that don't implement bank 0 (some non-conformant third-party gear like Mi-Boxer)
appear with `—` / `?` in the bank-0-derived columns but still get their `Random` and
device-type info from the dedicated queries.

`DaliBusScanner.cs` has a `READ_INTER_FRAME_DELAY_MS` constant (default `0`, well-tested
since the FW back-to-back-drop fix). Bump it to 100 ms as a workaround if you somehow
encounter a regression where scan results look shifted.

## EVG Mode IDs

| ID | Mode | Description |
|----|------|-------------|
| 1 | ONOFF | Relay/switch output |
| 2 | SINGLE | Single-channel PWM |
| 3 | CCT | 2-channel warm/cool white |
| 4 | RGB | 3-channel RGB PWM |
| 5 | RGBW | 4-channel RGBW PWM |
| 6 | WS2812 | Addressable LED strip (GRB) |
| 7 | SK6812_RGB | Addressable LED strip (GRB) |
| 8 | SK6812_RGBW | Addressable LED strip (GRBW) |

## Update Protocol (IEC 62386-105)

The update runs in 4 phases:

1. **Enter Bootloader** — Send QUERY FW UPDATE FEATURES (verify device supports updates), then START FW TRANSFER (config repeat, 2× within 100 ms). Device responds YES and resets into bootloader.

2. **Block 0 Validation** — Send BEGIN BLOCK 0 followed by TRANSFER BLOCK DATA frames containing GTIN, device key (EVG mode ID), and the pre-computed Fletcher-16 of the firmware payload at offsets `0x2C` / `0x2D`. The bootloader validates GTIN + mode against EEPROM identity. Updater polls `QUERY BLOCK FAULT` once at the end of this phase — that's the only window where the BL's `blockFault` flag can reflect a Block-0 fault (BEGIN BLOCK 1 wipes it).

3. **Firmware Transfer** — Send BEGIN BLOCK 1, then TRANSFER BLOCK DATA frames (3 firmware bytes per frame, ~3,600 frames for ~11 KB firmware). Data is staged in AT24C256 EEPROM (starting at 0x0080). **No mid-transfer polls** — they're structurally dead (BL's `blockFault` flag can only be set during Block-0 reception or at FINISH, never during Block-1 reception; payload integrity is verified inside the FINISH handler).

4. **Commit** — Send FINISH FW UPDATE (config repeat). The bootloader compares its accumulated Fletcher-16 against the expected value from Block 0:
   - Match: `flush_to_eeprom()` + `copy_eeprom_to_flash()` + auto-reboot into new firmware. Bus stays silent → updater sees `null` timeout → `SUCCESS`.
   - Mismatch: BL sends `0xFF` (= YES, "not done"), then auto-reboots into the existing firmware (flash untouched). Updater receives `0xFF` → reports `FAILED: Fletcher mismatch, re-run flash`.


## Project Structure

```
EVG-Updater/
├── EVG_Updater.slnx             Solution file (open in Visual Studio)
├── README.md                     This file
└── EVG_Updater/
    ├── EVG_Updater.csproj       .NET 8.0 WinForms project
    ├── Program.cs                Entry point — subcommand dispatch (GUI / flash / scan)
    ├── MainForm.cs               WinForms UI code-behind
    ├── MainForm.Designer.cs      WinForms UI layout
    ├── DaliGateway.cs            WebSocket client (background reader + in-flight pacing semaphore)
    ├── DaliBootloader.cs         IEC 62386-105 protocol state machine (4-phase update)
    └── DaliBusScanner.cs         Read-only bus scan (shorts 0..63, bank 0, random address)
```
