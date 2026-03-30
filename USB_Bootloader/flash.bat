@echo off
REM Flash bootloader + configurebootloader onto CH32V003 via WCH-LinkE
REM Double-click to flash. Requires WCH-LinkE connected.
REM
REM Flashing order:
REM   1. configurebootloader — sets option bytes to boot into bootloader
REM   2. bootloader          — writes USB HID bootloader to 1920-byte boot area

cd /d "%~dp0"

set WLINK=%USERPROFILE%\.platformio\packages\tool-wlink\wlink.exe

REM Check binaries exist (run deploy.bat first if missing)
if not exist "configurebootloader.bin" (
    echo ERROR: configurebootloader.bin not found. Run make_win.bat then deploy.bat first.
    pause
    exit /b 1
)
if not exist "bootloader.bin" (
    echo ERROR: bootloader.bin not found. Run make_win.bat then deploy.bat first.
    pause
    exit /b 1
)

REM ── Step 1: Flash configurebootloader (sets OB to boot-from-bootloader) ──
echo.
echo === Step 1: Flashing configurebootloader (option bytes setup) ===
"%WLINK%" flash "configurebootloader.bin"
if errorlevel 1 (
    echo.
    echo FLASH FAILED — configurebootloader
    pause
    exit /b 1
)
echo OK

REM Brief pause to let option bytes take effect after reset
timeout /t 2 /nobreak >nul

REM ── Step 2: Flash bootloader to boot area ──
echo.
echo === Step 2: Flashing bootloader (1920-byte boot area) ===
"%WLINK%" flash --address 0x1FFFF000 "bootloader.bin"
if errorlevel 1 (
    echo.
    echo FLASH FAILED — bootloader
    pause
    exit /b 1
)
echo OK

echo.
echo === All done — bootloader installed ===
echo Power-cycle the device. Hold BOOT button (PC7 low) during reset to enter bootloader.
echo.
pause
