# EVG-Flasher

Desktop app (C# / .NET, [Avalonia](https://avaloniaui.net/)) that provisions blank CH32V003-based EVG boards over the WCH-LinkE. Replaces the former `flash_blank_evg.ps1`.

> Trademark notice — see [root README](../README.md): *DALI*, *DALI-2* etc. are DiiA trademarks; this project is an independent IEC 62386 implementation, not DiiA-certified.

## What it does

Per chip, the same four steps as the old script:

1. Bootloader → `0x1FFFF000` (boot area, 1920 B fixed)
2. `configurebootloader` → `0x08000000` (writes the option bytes once)
3. Firmware → `0x08000000` (PlatformIO build + upload, overwrites step 2)
4. Verify option bytes (`RDPR`, `USER` complement, `FLASH_OBR`, `STARTMODE`)

**Auto mode** polls the WCH-Link once a second and runs the sequence on each chip as it is connected, then waits for removal — the bulk-provisioning loop.

## Configuration dropdown

The list is read from [`Firmware/platformio.ini`](../Firmware/platformio.ini) at runtime. Adding an `[env:...]` section there makes it appear here; no code change, no rebuild of this app. `default_envs` is marked.

For the addressable-strip modes an extra **LED count** field appears. It is injected as `PLATFORMIO_BUILD_FLAGS=-DWS2812_NUM_LEDS=<n>`, so `platformio.ini` stays untouched and the same environment serves any strip length.

## Requirements

- .NET SDK (the project targets `net10.0`)
- **WCH-LinkE** connected
- `wlink.exe` and `platformio.exe` under `%USERPROFILE%\.platformio\` (installed by the `ch32v` platform)
- `Bootloader/configurebootloader.bin`, plus a bootloader build — either `Bootloader/.pio/build/dali_bootloader/firmware.bin` or the legacy `Bootloader/dali_bootloader.bin`

The repository root is located by walking up from the executable until a folder holds both `Firmware/` and `Bootloader/`, so the app works from `bin/Debug` as well as from a published folder.

## Build & run

```bash
cd EVG-Flasher
dotnet run
```

## Not this tool

Updating firmware on an **already commissioned** EVG happens over the DALI bus with [EVG-Updater](../EVG-Updater/README.md). EVG-Flasher is for blank chips that still need bootloader and option bytes.
