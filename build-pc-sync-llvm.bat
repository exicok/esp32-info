@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem Re-run once through PowerShell so console output is also persisted to disk.
if not defined PC_SYNC_LOGGING (
  set "LOG_DIR=%~dp0logs"
  if not exist "%~dp0logs" mkdir "%~dp0logs"
  for /f %%T in ('powershell.exe -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "LOG_STAMP=%%T"
  set "PC_SYNC_LOGGING=1"
  set "PC_SYNC_SCRIPT=%~f0"
  set "PC_SYNC_ARGS=%*"
  set "PC_SYNC_LOG_FILE=%~dp0logs\pc-data-sync-build-!LOG_STAMP!.log"
  call :run_with_log
  exit /b !ERRORLEVEL!
)

rem LLVM-only Windows build. MinGW supplies Windows headers/import libraries only.
set "LLVM_ROOT=E:\SDK\LLVM"
set "MINGW_ROOT=E:\SDK\QT\Tools\mingw1310_64"
set "CLANG=%LLVM_ROOT%\bin\clang.exe"
set "LLVM_RC=%LLVM_ROOT%\bin\llvm-rc.exe"
set "OUTPUT=%~dp0pc_data_sync.exe"
set "BUILD_OUTPUT=%~dp0pc_data_sync.build.exe"
set "RESOURCE_RC=%TEMP%\pc_data_sync_%RANDOM%.rc"
set "RESOURCE_RES=%TEMP%\pc_data_sync_%RANDOM%.res"

if not exist "%CLANG%" (
  echo [ERROR] LLVM compiler not found: "%CLANG%"
  exit /b 1
)

if not exist "%LLVM_RC%" (
  echo [ERROR] LLVM resource compiler not found: "%LLVM_RC%"
  exit /b 1
)

if not exist "%MINGW_ROOT%\x86_64-w64-mingw32\include\windows.h" (
  echo [ERROR] Windows headers not found in: "%MINGW_ROOT%"
  echo Install the Qt MinGW 64-bit toolchain or update MINGW_ROOT in this script.
  exit /b 1
)

if not exist "%MINGW_ROOT%\x86_64-w64-mingw32\lib\libwinhttp.a" (
  echo [ERROR] Windows import libraries not found in: "%MINGW_ROOT%"
  exit /b 1
)

>"%RESOURCE_RC%" echo 1 24 "%~dp0pc_data_sync.manifest"

 echo Compiler: "%CLANG%"
 echo Linker: LLVM LLD
 echo Windows SDK: "%MINGW_ROOT%"
 echo Embedding application manifest...

"%LLVM_RC%" /nologo /fo "%RESOURCE_RES%" "%RESOURCE_RC%"
if errorlevel 1 goto :build_failed

echo Building pc_data_sync.c...
del /q "%BUILD_OUTPUT%" >nul 2>nul
"%CLANG%" --target=x86_64-w64-windows-gnu ^
  --sysroot="%MINGW_ROOT%" -fuse-ld=lld -std=c11 -O2 -Wall -Wextra -Wno-unknown-pragmas -mwindows ^
  -o "%BUILD_OUTPUT%" "%~dp0pc_data_sync.c" "%RESOURCE_RES%" ^
  -lsetupapi -ladvapi32 -lshell32 -lgdi32 -luser32 -lpdh -lpowrprof -lwinhttp -lws2_32 -lole32 -luuid -lm
if errorlevel 1 goto :build_failed

move /y "%BUILD_OUTPUT%" "%OUTPUT%" >nul 2>nul
if errorlevel 1 (
  del /q "%RESOURCE_RC%" "%RESOURCE_RES%" >nul 2>nul
  echo.
  echo [ERROR] LLVM build succeeded, but "%OUTPUT%" is currently in use.
  echo Close pc_data_sync.exe and run this script again.
  echo New executable kept at: "%BUILD_OUTPUT%"
  exit /b 1
)

del /q "%RESOURCE_RC%" "%RESOURCE_RES%" >nul 2>nul
echo.
echo Build complete: "%OUTPUT%" ^(LLVM clang + LLD^)
exit /b 0

:build_failed
 del /q "%RESOURCE_RC%" "%RESOURCE_RES%" >nul 2>nul
 echo.
 echo Build failed. Review the LLVM diagnostics above.
 exit /b 1

:run_with_log
echo Build log: "%PC_SYNC_LOG_FILE%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "& { $command = ('""{0}"" {1}' -f $env:PC_SYNC_SCRIPT, $env:PC_SYNC_ARGS); & $env:ComSpec /d /s /c $command 2>&1 | Tee-Object -LiteralPath $env:PC_SYNC_LOG_FILE; exit $LASTEXITCODE }"
exit /b %ERRORLEVEL%
