# DALI Bootloader Emergency Flasher

A single-purpose CH32V003 firmware that replaces the DALI bootloader in the
BOOT area (`0x1FFFF000`, 1920 B fixed). It exists for one scenario: **a bug is
found in a bootloader that is already deployed in the field**, where physical
access with a WCH-LinkE is impractical.

> Trademark notice — see [root README](../README.md): *DALI*, *DALI-2* etc. are
> DiiA trademarks; this project is an independent IEC 62386 implementation, not
> DiiA-certified.

In the normal case you never need this: the bootloader is validated once and
then flashed with the WCH-LinkE during commissioning (`flash_blank_evg.ps1`).

## Why this is a separate firmware

Writing the BOOT area from user code requires an **active debug module**. This
is undocumented, was established empirically (see below), and there is no
guarantee it survives future chip revisions. That makes it unfit for the
application firmware — where it also cost ~1.7 KB of a tight flash budget.

So the capability lives here instead, in a throw-away image that is flashed,
does its job, and is immediately replaced.

## The procedure

1. Flash **this** firmware onto the target over the DALI bus, using the
   existing (old) bootloader — an ordinary firmware update, no special
   protocol:
   `EVG_Updater flash BL-Emergency-Flasher.bin --addr <n>`
2. It runs automatically on boot: verifies the embedded image, erases the boot
   area, programs it, verifies by read-back, and reports on UART
   (PD5, 115200) — `BL FLASH OK` or `BL FLASH FAILED`, repeated forever.
3. Flash the regular application firmware back over the bus, now through the
   **new** bootloader:
   `EVG_Updater flash firmware.bin --addr <n>`

Being flashed *is* the trigger — there is no command to send. On failure the
old bootloader is still in place: the embedded image is checksum-verified
before the first erase, so a corrupt build cannot leave the device without a
bootloader.

## The bootloader image is compiled in

`embed_bootloader.py` runs as a PlatformIO pre-build script. It reads
`../Bootloader/.pio/build/dali_bootloader/firmware.bin` (falling back to the
`build.bat` legacy output `../Bootloader/dali_bootloader.bin`) and generates
`src/bl_image.h` — a C byte array plus its size and Fletcher-16.

That means the bootloader never travels over the bus as payload: no EEPROM
staging, no 642 block-data frames, no transfer checksum, no frame-pacing
issues. Build the bootloader first, then build this:

```bash
cd ../Bootloader && pio run     # produces the .bin that gets embedded
cd ../BL-Emergency-Flasher && pio run
```

The script aborts the build if the image is missing or exceeds 1920 bytes.

## Why an active debug module is required

Established on hardware, 2026-08-15. Without an active debug module, FPEC
erase and program operations on `0x1FFFF000` are **silent no-ops**: `EOP` is
set, `WRPRTERR` stays clear, and the flash contents simply do not change.
There is no error signal of any kind.

Ruled out along the way — none of these made any difference:

| Hypothesis | Result |
|---|---|
| Unlock order (`BOOT_MODEKEYR` before `MODEKEYR`) | no effect |
| Interrupts disabled during the operation | no effect |
| Routines executing from RAM (`section(".data")`) | no effect; additionally hung the flash macro (BSY never cleared), which also blocks the debug module |
| **Debug module active** (`wlink sdi-print enable`) | **works — first try, verified byte-for-byte via SWD dump** |

This matches the widespread claim that the BOOT area is "inherently
non-writable from user application code" — that holds precisely as long as no
debugger is attached.

## Status and remaining work

The flash path is implemented and the project builds (~4.6 KB of 16 KB, the
embedded bootloader included). Two pieces are still open:

- **`swio_self_unlock()` in `src/main.c` is a stub.** The CH32V003 can
  bit-bang its own single-wire debug protocol on PD1 and write `DMCONTROL` /
  `DMCFGR` / `DMSHDWCFGR`, enabling its own debug module without an external
  probe. Reference implementation: `swio.h` and `attempt_unlock()` in
  [monte-monte/ch32_user_bootloader_flasher](https://github.com/monte-monte/ch32_user_bootloader_flasher).
  Until this exists, a WCH-LinkE must be attached with the debug module
  enabled (`wlink sdi-print enable`) — which largely defeats the purpose,
  since you could then flash the bootloader over SWD directly.
- **Minimal DALI response.** After the bootloader has been replaced, the
  device must be able to re-enter the bootloader for step 3. Currently that
  needs the boot button (PA1 low at reset). Handling `START FW TRANSFER`
  (32-bit frame → set `FLASH->STATR` bit 14 → system reset) would make the
  whole sequence remote-only.

`reference/bl_writer.c` is the original over-the-bus implementation that used
to live in the application firmware (EEPROM staging, DALI framing, GTIN and
device-key checks). It does not build here — it is kept as a source of proven
detail, not as a component.
