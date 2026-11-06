@echo off
setlocal

if "%~1"=="" (
    echo Usage: %~nx0 ^<output_prefix^>
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
        --output "%out_dir%" ^
        --name "%name%"
) else (
    python "%~dp0dfu_release.py" ^
        --application "%prefix%.hex" ^
        --output "%out_dir%" ^
        --name "%name%" ^
        --resources-dir "%DFU_RESOURCES_DIR%"
)
exit /b %errorlevel%
