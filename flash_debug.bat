@echo off
:: nRF52 Windows Local Debug Flashing Script (56 KiB APP)
:: Purpose: Rebuild Keil project, erase chip (including UICR), flash SoftDevice and debug Application, then reset.
:: Internal FDS is disabled; application records use external Flash.

echo =========================================
echo 1. Rebuilding Keil Application (nRF52811_xxAA)...
echo =========================================

:: Path to Keil UV4
set UV4="D:\STM32\Core\UV4\UV4.exe"

if not exist %UV4% (
    echo Error: Keil UV4.exe not found at %UV4%. Please update the path in this script.
    pause
    exit /b 1
)

:: Run build. Keil will write output to Keil_Build_Log.txt
%UV4% -b Keil\EPD-nRF52.uvprojx -t nRF52811_xxAA -o Keil_Build_Log.txt
rem Keil returns 1 for a successful build with warnings, and >=2 for errors.
if errorlevel 2 (
    if exist Keil_Build_Log.txt type Keil_Build_Log.txt
    echo Error: Keil application build failed; flashing stopped.
    exit /b 1
)
type Keil_Build_Log.txt

echo =========================================
echo 2. Erasing entire chip (clearing UICR bootloader address)...
echo =========================================
nrfjprog -f nrf52 --eraseall
if %errorlevel% neq 0 (
    echo Error: Chip erase failed.
    pause
    exit /b 1
)

echo =========================================
echo 3. Programming SoftDevice S112...
echo =========================================
nrfjprog -f nrf52 --program SDK\17.1.0_ddde560\components\softdevice\s112\hex\s112_nrf52_7.3.0_softdevice.hex --sectorerase
if %errorlevel% neq 0 (
    echo Error: SoftDevice programming failed.
    pause
    exit /b 1
)

echo =========================================
echo 4. Programming 56 KiB Debug Application...
echo =========================================
nrfjprog -f nrf52 --program Keil\_build_nRF52\EPD-nRF52.hex --sectorerase
if %errorlevel% neq 0 (
    echo Error: Application programming failed.
    pause
    exit /b 1
)

echo =========================================
echo 5. Resetting and running...
echo =========================================
nrfjprog -f nrf52 --reset

echo =========================================
echo Flashing complete! (Application size up to 56 KiB)
echo Internal FDS is disabled; application records use external Flash.
echo =========================================
pause
