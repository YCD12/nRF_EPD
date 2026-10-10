@echo off
setlocal EnableExtensions

rem Build every external-Flash ERS package without writing to a device.
rem Usage:
rem   build-external-resources.bat [revision] [output_dir] [EPD-MIG1.json] [calendar-data.json]
rem Examples:
rem   build-external-resources.bat
rem   build-external-resources.bat 2 output\resources-r2 device-migration.json
rem   build-external-resources.bat 3 output\resources-r3 "" calendar-2027.json

set "REVISION=%~1"
if "%REVISION%"=="" set "REVISION=1"

set "BUILD_STAMP="
for /f %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "BUILD_STAMP=%%T"
if not defined BUILD_STAMP set "BUILD_STAMP=unknown"

set "OUTPUT_DIR=%~2"
if "%OUTPUT_DIR%"=="" set "OUTPUT_DIR=output\external-resources-r%REVISION%-%BUILD_STAMP%"

set "MIGRATION_BUNDLE=%~3"
set "CALENDAR_DATA=%~4"
set "CALENDAR_OPTION="
if not "%CALENDAR_DATA%"=="" (
    set CALENDAR_OPTION=--calendar-data "%CALENDAR_DATA%"
)
set "PROJECT_ROOT=%~dp0"

pushd "%PROJECT_ROOT%" || exit /b 1

if not "%CALENDAR_DATA%"=="" if not exist "%CALENDAR_DATA%" (
    echo Error: calendar data file was not found: "%CALENDAR_DATA%"
    popd
    exit /b 1
)

where python >nul 2>nul
if errorlevel 1 (
    echo Error: Python was not found on PATH.
    popd
    exit /b 1
)

echo =========================================
echo Building external Flash resources
echo   Revision : %REVISION%
echo   Output   : %OUTPUT_DIR%
if "%MIGRATION_BUNDLE%"=="" (
    echo   Records  : factory-default EPD config and empty annotations
) else (
    echo   Records  : migration bundle "%MIGRATION_BUNDLE%"
)
if not "%CALENDAR_DATA%"=="" echo   Calendar : "%CALENDAR_DATA%"
echo =========================================

if "%MIGRATION_BUNDLE%"=="" (
    python tools\resource_pack.py --check --fonts --calendar %CALENDAR_OPTION% --ui --todo --records ^
        --revision "%REVISION%" --output "%OUTPUT_DIR%"
) else (
    if not exist "%MIGRATION_BUNDLE%" (
        echo Error: migration bundle was not found: "%MIGRATION_BUNDLE%"
        popd
        exit /b 1
    )
    python tools\resource_pack.py --check --fonts --calendar %CALENDAR_OPTION% --ui --todo ^
        --migration-bundle "%MIGRATION_BUNDLE%" ^
        --revision "%REVISION%" --output "%OUTPUT_DIR%"
)
if errorlevel 1 (
    echo External resource generation failed.
    popd
    exit /b 1
)

echo.
echo Generated ERS packages:
for %%F in ("%OUTPUT_DIR%\*.ers") do echo   %%~nxF
echo.
echo Upload order: partition-table, calendar, ui, fonts, epd-config, calendar-annotations, todo.
echo Generation completed. No device Flash was written.
if /I "%OUTPUT_DIR:~0,7%"=="output\" (
    > "output\external-resources-latest.txt" echo %OUTPUT_DIR:~7%
)
popd
exit /b 0
