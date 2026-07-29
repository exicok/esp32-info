@echo off
setlocal
cd /d "%~dp0"

set "CLANG_CL="
set "VS_INSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS_INSTALL=%%I"
)

where clang-cl.exe >nul 2>nul
if not errorlevel 1 set "CLANG_CL=clang-cl.exe"

if not defined CLANG_CL if exist "%ProgramFiles%\LLVM\bin\clang-cl.exe" (
  set "CLANG_CL=%ProgramFiles%\LLVM\bin\clang-cl.exe"
)

if not defined CLANG_CL if exist "%LOCALAPPDATA%\Programs\LLVM\bin\clang-cl.exe" (
  set "CLANG_CL=%LOCALAPPDATA%\Programs\LLVM\bin\clang-cl.exe"
)

if not defined CLANG_CL if defined VS_INSTALL if exist "%VS_INSTALL%\VC\Tools\Llvm\x64\bin\clang-cl.exe" (
  set "CLANG_CL=%VS_INSTALL%\VC\Tools\Llvm\x64\bin\clang-cl.exe"
)

if not defined CLANG_CL if defined VS_INSTALL if exist "%VS_INSTALL%\VC\Tools\Llvm\bin\clang-cl.exe" (
  set "CLANG_CL=%VS_INSTALL%\VC\Tools\Llvm\bin\clang-cl.exe"
)

if not defined CLANG_CL (
  echo LLVM clang-cl was not found.
  echo Install the Visual Studio LLVM/Clang component or standalone LLVM, then run this script again.
  exit /b 1
)

if defined VS_INSTALL if exist "%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat" (
  call "%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
  if errorlevel 1 exit /b 1
)

echo Compiler: "%CLANG_CL%"
echo Building tray background pc_data_sync.c...

"%CLANG_CL%" /nologo /std:c11 /utf-8 /O2 /W4 ^
  /Fe:"%~dp0pc_data_sync.exe" "%~dp0pc_data_sync.c" ^
  /link /SUBSYSTEM:WINDOWS setupapi.lib advapi32.lib shell32.lib gdi32.lib user32.lib pdh.lib powrprof.lib winhttp.lib ^
  /MANIFEST:EMBED /MANIFESTINPUT:"%~dp0pc_data_sync.manifest"

if errorlevel 1 (
  echo.
  echo Build failed. If Windows headers or libraries are missing, run this script
  echo from an x64 Native Tools Command Prompt with LLVM installed.
  exit /b 1
)

echo.
echo Build complete: "%~dp0pc_data_sync.exe" ^(tray icon, no app window, no console^)
exit /b 0
