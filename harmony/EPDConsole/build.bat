@echo off
setlocal
set ROOT=%~dp0

if "%JAVA_HOME%"=="" (
  echo [ERROR] JAVA_HOME is not set. Point it to a JDK 17 installation.
  exit /b 1
)
if "%DEVECO_SDK_HOME%"=="" (
  echo [ERROR] DEVECO_SDK_HOME is not set. Point it to the OpenHarmony SDK (API 12).
  exit /b 1
)

set PATH=%JAVA_HOME%\bin;%PATH%
cd /d %ROOT%
node node_modules\@ohos\hvigor\bin\hvigor.js assembleHap --mode module -p product=default -p buildMode=debug --no-daemon
if errorlevel 1 exit /b 1
echo Build OK: %ROOT%entry\build\default\outputs\default\entry-default-unsigned.hap
