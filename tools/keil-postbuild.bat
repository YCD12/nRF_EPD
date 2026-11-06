@echo off
rem Keil After Make hook for the application project.
rem Generates the directly flashable full HEX and the OTA DFU package from the
rem application HEX that uVision just produced.
rem   %1 = uVision $L (linker output directory, usually trailing backslash)
rem   %2 = uVision @L (linker output name without extension)
setlocal

set "OUT_DIR=%~1"
set "OUT_NAME=%~2"
if "%OUT_DIR%"=="" (
    echo Error: keil-postbuild: missing output directory argument.
    exit /b 1
)
if "%OUT_NAME%"=="" (
    echo Error: keil-postbuild: missing output name argument.
    exit /b 1
)

for %%I in ("%~dp0..") do set "REPO=%%~fI"
set "APP_HEX=%OUT_DIR%%OUT_NAME%.hex"
for %%I in ("%APP_HEX%") do set "APP_HEX=%%~fI"

if not exist "%APP_HEX%" (
    echo Error: keil-postbuild: application HEX not found:
    echo   "%APP_HEX%"
    exit /b 1
)

set "NRFUTIL=%REPO%\tools\windows_tools\nrfutil.exe"
set "MERGEHEX=%REPO%\tools\windows_tools\mergehex.exe"
if not exist "%NRFUTIL%" (
    echo Error: keil-postbuild: nrfutil.exe not found at "%NRFUTIL%"
    exit /b 1
)
if not exist "%MERGEHEX%" (
    echo Error: keil-postbuild: mergehex.exe not found at "%MERGEHEX%"
    exit /b 1
)

set "PYTHON=python"
where python >nul 2>nul || set "PYTHON=py -3"

if not exist "%REPO%\_build" mkdir "%REPO%\_build"

pushd "%REPO%"
%PYTHON% "tools\dfu_release.py" ^
    --application "%APP_HEX%" ^
    --output "%REPO%\_build" ^
    --name "nrf52811_xxaa" ^
    --key "tools\priv.pem" ^
    --softdevice "SDK\17.1.0_ddde560\components\softdevice\s112\hex\s112_nrf52_7.3.0_softdevice.hex" ^
    --bootloader-config "SDK\17.1.0_ddde560\dfu\sdk_config.h" ^
    --nrfutil "%NRFUTIL%" ^
    --mergehex "%MERGEHEX%"
set "RESULT=%ERRORLEVEL%"
for /f "delims=" %%V in ('%PYTHON% "tools\dfu_release.py" --print-application-version') do set "DFU_VERSION=%%V"
popd

if not "%RESULT%"=="0" (
    echo Error: keil-postbuild: full HEX / OTA package generation failed.
    exit /b %RESULT%
)
if not defined DFU_VERSION (
    echo Error: keil-postbuild: could not resolve the DFU application version.
    exit /b 1
)

echo.
echo Full firmware: %REPO%\_build\nrf52811_xxaa-v%DFU_VERSION%-full.hex
echo OTA package:   %REPO%\_build\nrf52811_xxaa-v%DFU_VERSION%-ota.zip
exit /b 0
