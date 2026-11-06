@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Native Windows equivalent of build.sh.
rem Usage: build.bat [clean|build|incremental|test|ota|flash|flash_full|flash_softdevice|erase|rebuild|size|info] [MAKE_VAR=VALUE ...]
rem No argument builds a full business firmware with external-Flash writes enabled.
rem The directly flashable full HEX is generated without auditing the Bootloader
rem signing key.
rem Firmware compilation defaults to one Make job for reliable Windows builds.
rem Set BUILD_JOBS to opt in to parallel compilation. A trailing -jN cannot
rem override this setting, so build.bat build -j8 remains a one-job build.

pushd "%~dp0" || exit /b 1
set "ACTION=build"
set "MAKE_EXTRA="
set "SOFTDEVICE_HEX=SDK\17.1.0_ddde560\components\softdevice\s112\hex\s112_nrf52_7.3.0_softdevice.hex"

if /I "%~1"=="clean" goto :select_clean
if /I "%~1"=="build" goto :select_build
if /I "%~1"=="incremental" goto :select_incremental
if /I "%~1"=="test" goto :select_test
if /I "%~1"=="ota" goto :selectota
if /I "%~1"=="flash" goto :select_flash
if /I "%~1"=="flash_full" goto :select_flash_full
if /I "%~1"=="flash_softdevice" goto :select_flash_softdevice
if /I "%~1"=="erase" goto :select_erase
if /I "%~1"=="rebuild" goto :select_rebuild
if /I "%~1"=="size" goto :select_size
if /I "%~1"=="info" goto :select_info
goto :collect_args

:select_clean
set "ACTION=clean"
shift
goto :collect_args
:select_build
set "ACTION=build"
shift
goto :collect_args
:select_incremental
set "ACTION=incremental"
shift
goto :collect_args
:select_test
set "ACTION=test"
shift
goto :collect_args
:selectota
set "ACTION=ota"
shift
goto :collect_args
:select_flash
set "ACTION=flash"
shift
goto :collect_args
:select_flash_full
set "ACTION=flash_full"
shift
goto :collect_args
:select_flash_softdevice
set "ACTION=flash_softdevice"
shift
goto :collect_args
:select_erase
set "ACTION=erase"
shift
goto :collect_args
:select_rebuild
set "ACTION=rebuild"
shift
goto :collect_args
:select_size
set "ACTION=size"
shift
goto :collect_args
:select_info
set "ACTION=info"
shift

:collect_args
if "%~1"=="" goto :dispatch
set "MAKE_EXTRA=!MAKE_EXTRA! "%~1""
shift
goto :collect_args

:dispatch
if /I "%ACTION%"=="clean" (
    call :clean
    if errorlevel 1 goto :failed
    goto :success
)
if /I "%ACTION%"=="test" (
    call :run_host_tests
    if errorlevel 1 goto :failed
    goto :success
)
if /I "%ACTION%"=="flash_softdevice" (
    call :flash_softdevice
    if errorlevel 1 goto :failed
    goto :success
)
if /I "%ACTION%"=="erase" (
    call :erase
    if errorlevel 1 goto :failed
    goto :success
)

call :initialize_build_tools
if errorlevel 1 goto :failed

if /I "%ACTION%"=="build" (
    call :build %MAKE_EXTRA%
    if errorlevel 1 goto :failed
    goto :success
)
if /I "%ACTION%"=="incremental" (
    call :incremental %MAKE_EXTRA%
    if errorlevel 1 goto :failed
    goto :success
)
if /I "%ACTION%"=="rebuild" (
    call :build %MAKE_EXTRA%
    if errorlevel 1 goto :failed
    goto :success
)
if /I "%ACTION%"=="ota" (
    call :ota %MAKE_EXTRA%
    if errorlevel 1 goto :failed
    goto :success
)
if /I "%ACTION%"=="flash" (
    call :flash %MAKE_EXTRA%
    if errorlevel 1 goto :failed
    goto :success
)
if /I "%ACTION%"=="flash_full" (
    call :flash_full %MAKE_EXTRA%
    if errorlevel 1 goto :failed
    goto :success
)
if /I "%ACTION%"=="size" (
    call :show_size
    if errorlevel 1 goto :failed
    goto :success
)
if /I "%ACTION%"=="info" (
    call :show_info
    goto :success
)

echo Error: unknown target "%ACTION%".
goto :failed

:success
echo =========================================
echo %ACTION% completed successfully.
echo =========================================
popd
exit /b 0

:failed
set "EXIT_CODE=%errorlevel%"
if "%EXIT_CODE%"=="0" set "EXIT_CODE=1"
echo Build failed.
popd
exit /b %EXIT_CODE%

:initialize_build_tools
set "GCC_EXE="
for /f "delims=" %%F in ('dir /s /b /a-d "tools\windows_tools\gcc\arm-none-eabi-gcc.exe" 2^>nul') do (
    if not defined GCC_EXE set "GCC_EXE=%%~fF"
)
if not defined GCC_EXE (
    echo Error: Windows ARM GCC was not found below tools\windows_tools\gcc.
    exit /b 1
)
for %%F in ("%GCC_EXE%") do set "GCC_BIN=%%~dpF"
set "GCC_BIN=%GCC_BIN:\=/%"
set "SIZE_EXE=%GCC_BIN%arm-none-eabi-size.exe"

rem Cygwin Make 4.x works with the native GCC when project paths are passed
rem in D:/ form. Fall back to any Make found on PATH.
set "MAKE_EXE="
if exist "D:\cygwin64\bin\make.exe" set "MAKE_EXE=D:\cygwin64\bin\make.exe"
if not defined MAKE_EXE (
    for %%M in (mingw32-make.exe make.exe) do (
        if not defined MAKE_EXE (
            for /f "delims=" %%P in ('where %%M 2^>nul') do if not defined MAKE_EXE set "MAKE_EXE=%%P"
        )
    )
)
if not defined MAKE_EXE (
    echo Error: GNU Make was not found. Install GNU Make and add make.exe to PATH.
    exit /b 1
)

set "PROJECT_ROOT=%CD%"
set "PROJECT_ROOT=%PROJECT_ROOT:\=/%"
set "SDK_ROOT=%PROJECT_ROOT%/SDK/17.1.0_ddde560"
set "TOOLS_ROOT=%PROJECT_ROOT%/tools"
if not defined BUILD_JOBS set "BUILD_JOBS=1"
set "FIRMWARE_PREFIX=_build\nrf52811_xxaa"
set "FIRMWARE_OUT=%FIRMWARE_PREFIX%.out"
set "FIRMWARE_HEX=%FIRMWARE_PREFIX%.hex"
rem FULL_HEX/SETTINGS_HEX carry the resolved DFU application version and are
rem built by :resolve_dfu_version (default APP_VERSION; override with
rem DFU_APPLICATION_VERSION for a hotfix release).
exit /b 0

:resolve_dfu_version
for /f "delims=" %%V in ('python "tools\dfu_release.py" --print-application-version') do set "DFU_VERSION=%%V"
if not defined DFU_VERSION (
    echo Error: could not resolve the DFU application version.
    exit /b 1
)
set "FULL_HEX=%FIRMWARE_PREFIX%-v%DFU_VERSION%-full.hex"
set "SETTINGS_HEX=%FIRMWARE_PREFIX%-v%DFU_VERSION%-settings.hex"
exit /b 0

:clean
echo =========================================
echo Cleaning build output...
echo =========================================
if not exist "_build" exit /b 0

rem Host-test executables may briefly be held by Defender, Explorer, or a
rem just-finished test process. Retry once and avoid noisy cmd error text.
rmdir /s /q "_build" >nul 2>nul
if exist "_build" (
    echo Some previous build files are still in use; retrying cleanup...
    powershell -NoProfile -NonInteractive -Command "Start-Sleep -Seconds 1" >nul 2>nul
    rmdir /s /q "_build" >nul 2>nul
)
if not exist "_build" exit /b 0

rem The directory itself can be locked by an ACL written by another tool
rem (for example an offline sandbox), even when its contents are writable and
rem no process is running. Empty it and reuse it instead of failing the build:
rem every object is recompiled when the .build-options stamp changes, and with
rem an empty directory no stale object can be mistaken for an up-to-date one.
echo Warning: _build cannot be removed as a directory ^(protected by another tool^).
echo          Clearing its contents and reusing it for this build.
del /f /s /q "_build\*.*" >nul 2>nul
for /d %%D in ("_build\*") do rmdir /s /q "%%~fD" >nul 2>nul
set "BUILD_LEFTOVERS="
for /f %%F in ('dir /b "_build" 2^>nul') do set "BUILD_LEFTOVERS=1"
if defined BUILD_LEFTOVERS (
    echo Error: _build is protected and its old contents cannot be cleared.
    echo        Remove it from an elevated shell, then retry:
    echo   takeown /f "_build" /r /d y ^&^& icacls "_build" /grant "%%USERNAME%%":F ^&^& rmdir /s /q "_build"
    exit /b 1
)
rem Verify the reused directory is writable; otherwise compilation would fail
rem later with a confusing linker/tool error.
> "_build\.write_probe" echo probe
if not exist "_build\.write_probe" (
    echo Error: _build is not writable. Remove it from an elevated shell:
    echo   takeown /f "_build" /r /d y ^&^& icacls "_build" /grant "%%USERNAME%%":F ^&^& rmdir /s /q "_build"
    exit /b 1
)
del /f /q "_build\.write_probe" >nul 2>nul
echo Note: _build is now empty but the protected directory remains.
echo       To remove it permanently, run in an elevated terminal:
echo   takeown /f "_build" /r /d y ^&^& icacls "_build" /grant "%%USERNAME%%":F ^&^& rmdir /s /q "_build"
exit /b 0

:make_firmware
echo =========================================
echo Building Make target: nrf52811_xxaa
echo GCC : %GCC_EXE%
echo Make: %MAKE_EXE%
echo Jobs: %BUILD_JOBS% ^(set BUILD_JOBS to change; -jN is ignored^)
echo =========================================
"%MAKE_EXE%" -j%BUILD_JOBS% -f Makefile.nRF52_linux nrf52811_xxaa ^
    "PROJ_DIR=%PROJECT_ROOT%" ^
    "SDK_ROOT=%SDK_ROOT%" ^
    "TOOLS_DIR=%TOOLS_ROOT%" ^
    "GNU_INSTALL_ROOT=%GCC_BIN%" ^
    "MAKE=make.exe" ^
    "PASS_LINKER_INPUT_VIA_FILE=0" %* -j%BUILD_JOBS%
exit /b %errorlevel%

:ensure_firmware
if exist "%FIRMWARE_OUT%" exit /b 0
call :make_firmware
exit /b %errorlevel%

:run_host_tests
set "HOST_BASH=D:\cygwin64\bin\bash.exe"
if not exist "%HOST_BASH%" (
    echo Error: Cygwin Bash was not found at %HOST_BASH%.
    echo The build and test targets mirror build.sh and require Cygwin.
    exit /b 1
)
set "HOST_ROOT=%CD%"
set "HOST_ROOT=%HOST_ROOT:\=/%"
set "HOST_ROOT=/cygdrive/%HOST_ROOT:~0,1%%HOST_ROOT:~2%"
echo =========================================
echo Running host tests...
echo =========================================
"%HOST_BASH%" "%HOST_ROOT%/tests/run_host_tests.sh"
exit /b %errorlevel%

:build
call :clean
if errorlevel 1 exit /b %errorlevel%
call :compile %*
if errorlevel 1 exit /b %errorlevel%
call :merge_full_firmware
exit /b %errorlevel%

:incremental
call :compile %*
exit /b %errorlevel%

:compile
call :run_host_tests
if errorlevel 1 exit /b 1
call :make_firmware %*
if errorlevel 1 exit /b %errorlevel%
call :show_size
if errorlevel 1 exit /b %errorlevel%
call :show_memory
if errorlevel 1 exit /b %errorlevel%
call :build_external_resources
exit /b %errorlevel%

:show_size
call :ensure_firmware
if errorlevel 1 exit /b %errorlevel%
"%SIZE_EXE%" "%FIRMWARE_OUT%"
exit /b %errorlevel%

:show_memory
call :ensure_firmware
if errorlevel 1 exit /b %errorlevel%
set /a TEXT=0, DATA=0, FS_DATA=0, BSS=0, FLASH_META=0
"%SIZE_EXE%" -A "%FIRMWARE_OUT%" > "_build\size_sections.txt"
if errorlevel 1 exit /b %errorlevel%
for /f "tokens=1,2" %%A in (_build\size_sections.txt) do (
    if "%%A"==".text" set /a TEXT=%%B
    if "%%A"==".data" set /a DATA=%%B
    if "%%A"==".fs_data" set /a FS_DATA=%%B
    if "%%A"==".bss" set /a BSS=%%B
    if "%%A"==".sdh_soc_observers" set /a FLASH_META+=%%B
    if "%%A"==".sdh_ble_observers" set /a FLASH_META+=%%B
    if "%%A"==".sdh_req_observers" set /a FLASH_META+=%%B
    if "%%A"==".sdh_state_observers" set /a FLASH_META+=%%B
    if "%%A"==".sdh_stack_observers" set /a FLASH_META+=%%B
    if "%%A"==".ARM.exidx" set /a FLASH_META+=%%B
)
del /q "_build\size_sections.txt" >nul 2>nul
set /a FLASH_LIMIT=57344, RAM_LIMIT=15656, STACK_RESERVED=2048
set /a FLASH_USED=TEXT+DATA+FS_DATA+FLASH_META
set /a STATIC_RAM=DATA+FS_DATA+BSS
set /a RAM_RESERVED=STATIC_RAM+STACK_RESERVED
set /a FLASH_FREE=FLASH_LIMIT-FLASH_USED
set /a RAM_FREE=RAM_LIMIT-RAM_RESERVED
echo === Memory budget ^(nRF52811 application layout^) ===
echo   Flash: !FLASH_USED! / !FLASH_LIMIT! bytes ^(free !FLASH_FREE!^)
echo   RAM static: !STATIC_RAM! bytes ^(.data + .fs_data + .bss^)
echo   RAM reserved: !RAM_RESERVED! / !RAM_LIMIT! bytes ^(includes !STACK_RESERVED!-byte stack; free !RAM_FREE!^)
exit /b 0

:ota
call :clean
if errorlevel 1 exit /b %errorlevel%
call :compile %*
if errorlevel 1 exit /b %errorlevel%
echo =========================================
echo Generating OTA package...
echo =========================================
if not exist "tools\make-ota-nrf52.bat" (
    echo Error: tools\make-ota-nrf52.bat was not found.
    exit /b 1
)
call "%CD%\tools\make-ota-nrf52.bat" "%FIRMWARE_PREFIX%"
if errorlevel 1 exit /b %errorlevel%
call :resolve_dfu_version
if errorlevel 1 exit /b %errorlevel%
echo Full firmware: %FULL_HEX%
echo OTA package:   %FIRMWARE_PREFIX%-v%DFU_VERSION%-ota.zip
echo Settings HEX:  %SETTINGS_HEX%
echo Release JSON:  %FIRMWARE_PREFIX%-v%DFU_VERSION%-release.json
exit /b 0

:build_external_resources
echo =========================================
echo Building external Flash resources...
echo =========================================
if not exist "build-external-resources.bat" (
    echo Error: build-external-resources.bat was not found.
    exit /b 1
)
call "%CD%\build-external-resources.bat"
exit /b %errorlevel%

:merge_full_firmware
set "MERGE_NRFUTIL_EXE="
set "MERGEHEX_EXE="
if exist "tools\windows_tools\nrfutil.exe" set "MERGE_NRFUTIL_EXE=%CD%\tools\windows_tools\nrfutil.exe"
if exist "tools\windows_tools\mergehex.exe" set "MERGEHEX_EXE=%CD%\tools\windows_tools\mergehex.exe"
if not defined MERGE_NRFUTIL_EXE (
    for /f "delims=" %%P in ('where nrfutil.exe 2^>nul') do if not defined MERGE_NRFUTIL_EXE set "MERGE_NRFUTIL_EXE=%%P"
)
if not defined MERGEHEX_EXE (
    for /f "delims=" %%P in ('where mergehex.exe 2^>nul') do if not defined MERGEHEX_EXE set "MERGEHEX_EXE=%%P"
)
if not defined MERGE_NRFUTIL_EXE (
    echo Error: nrfutil.exe was not found under tools\windows_tools or on PATH.
    exit /b 1
)
if not defined MERGEHEX_EXE (
    echo Error: mergehex.exe was not found under tools\windows_tools or on PATH.
    exit /b 1
)
if not exist "%FIRMWARE_HEX%" (
    echo Error: application HEX was not generated: %FIRMWARE_HEX%
    exit /b 1
)
if not exist "%SOFTDEVICE_HEX%" (
    echo Error: SoftDevice HEX was not found: %SOFTDEVICE_HEX%
    exit /b 1
)
set "BOOTLOADER_HEX=Keil\_build_DFU52\bl_nrf52811_xxaa_s112.hex"
if not exist "!BOOTLOADER_HEX!" set "BOOTLOADER_HEX=output\bootloader-8k\bl_nrf52811_xxaa_s112_8k.hex"
if not exist "!BOOTLOADER_HEX!" set "BOOTLOADER_HEX=tools\bootloader\bl_nrf52811_xxaa_s112_8k.hex"
if not exist "!BOOTLOADER_HEX!" (
    echo Error: Bootloader HEX was not found.
    exit /b 1
)
call :resolve_dfu_version
if errorlevel 1 exit /b %errorlevel%
python "tools\dfu_release.py" --audit-only --application "%FIRMWARE_HEX%" --bootloader "!BOOTLOADER_HEX!" --softdevice "%SOFTDEVICE_HEX%"
if errorlevel 1 exit /b %errorlevel%
echo =========================================
echo Generating directly flashable full firmware...
echo Bootloader: !BOOTLOADER_HEX!
echo DFU application version: %DFU_VERSION%
echo Key compatibility audit: skipped for build target
echo =========================================
"!MERGE_NRFUTIL_EXE!" settings generate --family NRF52810 ^
    --application "%FIRMWARE_HEX%" ^
    --softdevice "%SOFTDEVICE_HEX%" ^
    --application-version %DFU_VERSION% ^
    --bootloader-version 1 ^
    --bl-settings-version 2 ^
    --app-boot-validation VALIDATE_GENERATED_CRC ^
    --start-address 0x2F000 ^
    "!SETTINGS_HEX!"
if errorlevel 1 exit /b %errorlevel%
"!MERGEHEX_EXE!" -m "%SOFTDEVICE_HEX%" "!BOOTLOADER_HEX!" "%FIRMWARE_HEX%" "!SETTINGS_HEX!" -o "%FULL_HEX%"
if errorlevel 1 exit /b %errorlevel%
if not exist "%FULL_HEX%" (
    echo Error: merged firmware was not generated: %FULL_HEX%
    exit /b 1
)
echo Full firmware: %FULL_HEX%
exit /b 0

:require_nrfjprog
set "NRFJPROG_EXE="
for /f "delims=" %%P in ('where nrfjprog.exe 2^>nul') do if not defined NRFJPROG_EXE set "NRFJPROG_EXE=%%P"
if not defined NRFJPROG_EXE (
    echo Error: nrfjprog.exe was not found. Install Nordic nRF Command Line Tools.
    exit /b 1
)
exit /b 0

:flash
rem Refresh application validation in both bootloader settings pages as well.
call :ota %*
if errorlevel 1 exit /b %errorlevel%
call :resolve_dfu_version
if errorlevel 1 exit /b %errorlevel%
call :require_nrfjprog
if errorlevel 1 exit /b 1
"%NRFJPROG_EXE%" -f nrf52 --program "%FIRMWARE_HEX%" --sectorerase --verify
if errorlevel 1 exit /b %errorlevel%
"%NRFJPROG_EXE%" -f nrf52 --program "%SETTINGS_HEX%" --sectorerase --verify
if errorlevel 1 exit /b %errorlevel%
"%NRFJPROG_EXE%" -f nrf52 --reset
exit /b %errorlevel%

:flash_full
call :ota %*
if errorlevel 1 exit /b %errorlevel%
call :resolve_dfu_version
if errorlevel 1 exit /b %errorlevel%
call :require_nrfjprog
if errorlevel 1 exit /b 1
"%NRFJPROG_EXE%" -f nrf52 --program "%FULL_HEX%" --chiperase
if errorlevel 1 exit /b %errorlevel%
"%NRFJPROG_EXE%" -f nrf52 --reset
exit /b %errorlevel%

:flash_softdevice
call :require_nrfjprog
if errorlevel 1 exit /b 1
if not exist "%SOFTDEVICE_HEX%" (
    echo Error: SoftDevice hex was not found: %SOFTDEVICE_HEX%
    exit /b 1
)
"%NRFJPROG_EXE%" -f nrf52 --program "%SOFTDEVICE_HEX%" --sectorerase
if errorlevel 1 exit /b %errorlevel%
"%NRFJPROG_EXE%" -f nrf52 --reset
exit /b %errorlevel%

:erase
call :require_nrfjprog
if errorlevel 1 exit /b 1
"%NRFJPROG_EXE%" -f nrf52 --eraseall
exit /b %errorlevel%

:show_info
call :resolve_dfu_version
if errorlevel 1 exit /b %errorlevel%
echo Project:   EPD-nRF52
echo Target:    nrf52811_xxaa
echo GCC:       %GCC_BIN%
echo Make:      %MAKE_EXE%
echo Jobs:      %BUILD_JOBS%
echo SDK:       %SDK_ROOT%
echo Output:    _build
echo Build full: %FULL_HEX%
exit /b 0
