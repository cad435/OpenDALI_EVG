@echo off
REM Flash DALI bootloader onto CH32V003 via WCH-LinkE
REM Requires configurebootloader to have been run once before.

cd /d "%~dp0"

set WLINK=%USERPROFILE%\.platformio\packages\tool-wlink\wlink.exe

if not exist "dali_bootloader.bin" (
    echo ERROR: dali_bootloader.bin not found. Run build.bat first.
    pause
    exit /b 1
)

echo Flashing DALI bootloader to boot area (0x1FFFF000)...
"%WLINK%" flash --address 0x1FFFF000 "dali_bootloader.bin"
if errorlevel 1 (
    echo.
    echo FLASH FAILED
    pause
    exit /b 1
)

echo.
echo Done. Hold PC7 low during reset to enter DALI bootloader mode.
echo.
pause
