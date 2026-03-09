# USB Bootloader

> **WARNING: Do NOT connect USB while the EVG is connected to a DALI bus or mains power.**
> The USB data lines share GPIO pins with the microcontroller and are not galvanically isolated.
> Only use USB for firmware updates on a standalone board that is not integrated into a system.
> Connecting USB while the EVG is on a DALI bus risks damage to both the USB host and the EVG.

USB HID bootloader for the CH32V003, based on [cnlohr's ch32v003fun-usb-bootloader](https://github.com/cnlohr/ch32v003fun/tree/master/bootloader). Allows firmware updates over USB without a dedicated programmer after initial setup.

The bootloader source (`src/`) is not included in this repository. Clone it from cnlohr's repo if you need to rebuild.

## Pin Configuration

| Function | Pin | Notes |
|----------|-----|-------|
| USB D+ | PD4 | |
| USB D- | PD3 | |
| USB D- Pull-Up (DPU) | PD0 | Directly driven by bootloader |
| Boot Button | PC7 | Pull low during reset to enter bootloader |

## Boot Behaviour

- On power-up, the chip enters the bootloader (option bytes set to boot-from-bootloader)
- If the boot button (PC7) is **not** held low, the bootloader immediately jumps to user code
- If the boot button **is** held low, the bootloader stays active and enumerates as USB HID device (`VID:1209 PID:B003`)

## Initial Setup (requires WCH-LinkE programmer)

This only needs to be done once per chip.

1. Connect the WCH-LinkE programmer
2. Run `flash.bat`

This flashes two things in sequence:
1. **configurebootloader.bin** — a small program that sets the option bytes to boot-from-bootloader mode, then halts
2. **bootloader.bin** — the actual USB bootloader, written to the 1920-byte boot area at `0x1FFFF000`

## Rebuilding (optional)

<bootloader.c>
```
#define DISABLE_BOOTLOAD

#define BOOTLOADER_BTN_PORT D
#define BOOTLOADER_BTN_PIN 0
#define BOOTLOADER_BTN_TRIG_LEVEL 0

#define BOOTLOADER_TIMEOUT_PWR 0
```

<usb_config.h>
```
#define USB_PORT D
#define USB_PIN_DP 4
#define USB_PIN_DM 3

#define USB_PIN_DPU 0
```

## Flashing User Firmware via USB

After the bootloader is installed, firmware updates no longer require a programmer:

1. Hold the boot button and power-cycle (or reset) the device
2. The device enumerates as USB HID
3. Flash using [minichlink](https://github.com/cnlohr/ch32v003fun/tree/master/minichlink): `minichlink -w firmware.bin flash`
