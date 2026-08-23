@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ===========================================================================
rem  clean-upload.bat - remove build output before uploading clean sources.
rem
rem    clean-upload.bat          build output / compiler caches / temp files
rem    clean-upload.bat /all     also wipe music\data (database, cache, config)
rem    clean-upload.bat /dry     print what would be removed, delete nothing
rem    flags can be combined:    clean-upload.bat /all /dry
rem
rem  cmd twin of clean-upload.ps1 (no PowerShell execution policy needed) and
rem  additionally drops the CMake / Qt autogen intermediate folders.
rem  Only paths inside this script's own directory are touched, never .git.
rem ===========================================================================

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "DRY=0"
set "USERDATA=0"
set /a REMOVED=0

:parse
if "%~1"=="" goto run
if /I "%~1"=="/dry" set "DRY=1"
if /I "%~1"=="/all" set "USERDATA=1"
if /I "%~1"=="/?" goto usage
if /I "%~1"=="/help" goto usage
shift
goto parse

:usage
echo usage: clean-upload.bat [/all] [/dry]
echo   /all  also wipe music\data\{database,cache,settings} (keeps .gitkeep)
echo   /dry  preview only, delete nothing
exit /b 0

:run
echo === workspace: %ROOT%
if "%DRY%"=="1" echo === dry run, nothing will be deleted

for %%D in (
    ".pio"
    ".cache"
    "music\build"
    "music\dist"
    "music\.cache"
    "music\out"
    "music\cmake-build-debug"
    "music\cmake-build-release"
) do call :killdir "%ROOT%\%%~D"

for %%F in (
    "compile_commands.json"
    "pc_data_sync.exe"
    "pc_data_sync.build.exe"
    "music\compile_commands.json"
    "music\WANMUSIC.exe"
    "music\MusicPlayer.exe.lnk"
) do call :killfile "%ROOT%\%%~F"

rem compiler intermediates and logs
for /R "%ROOT%" %%F in (*.log *.tmp *.obj *.o *.ilk *.pdb *.exp *.res) do call :killfile "%%~fF"

rem Qt / CMake leftovers that can also appear outside music\build
for /F "delims=" %%D in ('dir /AD /B /S "%ROOT%" 2^>nul ^| findstr /I /E /C:"_autogen" /C:"\CMakeFiles"') do call :killdir "%%~fD"

if "%USERDATA%"=="1" (
    for %%U in ("music\data\database" "music\data\cache" "music\data\settings") do (
        if exist "%ROOT%\%%~U" (
            for /F "delims=" %%E in ('dir /B /A "%ROOT%\%%~U" 2^>nul') do (
                if /I not "%%~nxE"==".gitkeep" call :killany "%ROOT%\%%~U\%%~E"
            )
        )
    )
)

echo === done, %REMOVED% item(s) processed
exit /b 0

rem ------------------------------------------------------------- subroutines
:guard
rem exit 0 = safe to delete: inside the workspace and not part of .git
set "TARGET=%~f1"
set "REL=!TARGET:%ROOT%\=!"
if "!REL!"=="!TARGET!" exit /b 1
echo !TARGET! | findstr /I /C:"\.git" >nul && exit /b 1
exit /b 0

:killdir
call :guard "%~1" || exit /b 0
if not exist "%~1\" exit /b 0
if "%DRY%"=="1" (echo [dry] dir  %~1) else (rd /S /Q "%~1" 2>nul && echo [del] dir  %~1)
set /a REMOVED+=1
exit /b 0

:killfile
call :guard "%~1" || exit /b 0
if not exist "%~1" exit /b 0
if exist "%~1\" exit /b 0
if "%DRY%"=="1" (echo [dry] file %~1) else (del /F /Q "%~1" 2>nul && echo [del] file %~1)
set /a REMOVED+=1
exit /b 0

:killany
if exist "%~1\" (call :killdir "%~1") else (call :killfile "%~1")
exit /b 0
