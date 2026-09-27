@echo off
rem ---------------------------------------------------------------------------
rem  Nioh1 Perfect Guard - build / verify / package
rem
rem  Double-click this file, or run it from a prompt. It just forwards to
rem  build.ps1 with the execution policy bypassed for this process only
rem  (no system setting is changed).
rem
rem    build.cmd                  build, run every check, generate dist\
rem    build.cmd -Install         also copy into the game's mods\ folder
rem    build.cmd -SkipTests       skip the checks (debugging only)
rem    build.cmd -GameDir "D:\Nioh"
rem
rem  Note: this file is deliberately ASCII-only. cmd.exe does not understand a
rem  UTF-8 BOM, and without one it would mis-decode non-ASCII characters.
rem ---------------------------------------------------------------------------
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (
    echo.
    echo [build.cmd] BUILD FAILED - exit code %RC%
)
rem keep the window open when double-clicked without arguments
if "%~1"=="" pause
exit /b %RC%
