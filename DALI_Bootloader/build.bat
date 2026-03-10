@echo off
REM Build DALI bootloader for CH32V003 (1920-byte boot area)
REM Dependencies (ch32v003fun.h, libgcc.a) are in ch32v003fun/ subfolder.
REM Toolchain: PlatformIO's riscv-wch-elf-gcc, auto-detected via USERPROFILE.

cd /d "%~dp0"

set PREFIX=%USERPROFILE%\.platformio\packages\toolchain-riscv\bin\riscv-wch-elf
if not exist "%PREFIX%-gcc.exe" (
    echo ERROR: riscv-wch-elf-gcc not found at %PREFIX%-gcc.exe
    echo Install PlatformIO's toolchain-riscv package first.
    pause
    exit /b 1
)

set CH32V003FUN=ch32v003fun
set LDSCRIPT=bootloader.ld

echo Cleaning...
del /q dali_bootloader.bin dali_bootloader.elf dali_bootloader.lst dali_bootloader.map 2>nul

echo Building DALI bootloader...
"%PREFIX%-gcc" -o dali_bootloader.elf ^
  startup.S ^
  dali_bootloader.c ^
  -g -Os -flto -ffunction-sections -fdata-sections -fmessage-length=0 -msmall-data-limit=8 ^
  -march=rv32ec -mabi=ilp32e -DCH32V003=1 ^
  -static-libgcc -fno-builtin ^
  -I"%CH32V003FUN%" ^
  -nostdlib ^
  -I. -Wall ^
  -T "%LDSCRIPT%" -Wl,--gc-sections ^
  -Wl,--print-memory-usage ^
  -L"%CH32V003FUN%" -lgcc

if errorlevel 1 (
    echo.
    echo BUILD FAILED
    pause
    exit /b 1
)

"%PREFIX%-objcopy" -O binary dali_bootloader.elf dali_bootloader.bin
"%PREFIX%-objdump" -S dali_bootloader.elf > dali_bootloader.lst
"%PREFIX%-nm" --size-sort dali_bootloader.elf

echo.
for %%F in (dali_bootloader.bin) do echo dali_bootloader.bin: %%~zF bytes (max 1920)
echo.
pause
