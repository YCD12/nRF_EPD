@echo off
setlocal

if "%~1"=="" (
    echo Usage: set DFU_APPLICATION_VERSION=0x1A ^& set DFU_PREVIOUS_APPLICATION_VERSION=0x19
    echo        %~nx0 ^<output_prefix^>
    exit /b 1
)
if "%DFU_APPLICATION_VERSION%"=="" (
    echo Error: DFU_APPLICATION_VERSION must be a new monotonic Nordic DFU version.
    exit /b 1
)
if "%DFU_PREVIOUS_APPLICATION_VERSION%"=="" (
    echo Error: DFU_PREVIOUS_APPLICATION_VERSION must identify the prior released DFU version.
    exit /b 1
)

set "prefix=%~1"
for %%F in ("%prefix%") do (
    set "out_dir=%%~dpF"
    set "name=%%~nxF"
)
set "out_dir=%out_dir:~0,-1%"

if "%DFU_RESOURCES_DIR%"=="" (
    python "%~dp0dfu_release.py" ^
        --application "%prefix%.hex" ^
        --application-version "%DFU_APPLICATION_VERSION%" ^
        --previous-version "%DFU_PREVIOUS_APPLICATION_VERSION%" ^
        --output "%out_dir%" ^
        --name "%name%"
) else (
    python "%~dp0dfu_release.py" ^
        --application "%prefix%.hex" ^
        --application-version "%DFU_APPLICATION_VERSION%" ^
        --previous-version "%DFU_PREVIOUS_APPLICATION_VERSION%" ^
        --output "%out_dir%" ^
        --name "%name%" ^
        --resources-dir "%DFU_RESOURCES_DIR%"
)
exit /b %errorlevel%
