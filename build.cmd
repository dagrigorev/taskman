@echo off
rem ------------------------------------------------------------------------
rem build.cmd - cmd.exe entry point for the Classic Task Manager build.
rem
rem Forwards to build.ps1, which locates MSVC (or falls back to MinGW-w64)
rem on its own. No "Native Tools Command Prompt" required.
rem
rem   build              release build -> build\taskman.exe
rem   build rebuild      clean, then build
rem   build debug        unoptimised build with symbols
rem   build test         build and run the ten CTest suites
rem   build run          build, then launch it
rem   build clean        remove build\
rem
rem   build test -Strict         warnings become errors
rem   build -Toolchain mingw     force gcc/windres
rem ------------------------------------------------------------------------
setlocal

set "PS=powershell.exe"
where pwsh.exe >nul 2>&1 && set "PS=pwsh.exe"

"%PS%" -NoProfile -NoLogo -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
