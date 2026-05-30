@echo off
setlocal

set "out_dir=%~1"
set "out_name=%~2"
set "src_hex=%~dp0..\SDK\17.1.0_ddde560\components\softdevice\s112\hex\s112_nrf52_7.3.0_softdevice.hex"
set "dst_hex=%out_dir%%out_name%.hex"

if "%out_dir%"=="" (
    echo Error: missing output directory.
    exit /b 1
)

if "%out_name%"=="" (
    echo Error: missing output name.
    exit /b 1
)

if not exist "%src_hex%" (
    echo Error: SoftDevice hex not found:
    echo   "%src_hex%"
    exit /b 1
)

if not exist "%out_dir%" mkdir "%out_dir%"

copy /Y "%src_hex%" "%dst_hex%" >nul
if errorlevel 1 (
    echo Error: failed to stage SoftDevice hex to:
    echo   "%dst_hex%"
    exit /b 1
)

echo Staged SoftDevice hex at "%dst_hex%"
exit /b 0
