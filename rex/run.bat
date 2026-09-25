@echo off
rem Runs the recompiled Rayman Origins (ReXGlue runtime) on Windows.
rem
rem Usage: rex\run.bat [extra ReXGlue options]
rem
rem Keyboard (Xbox/PlayStation controllers work out of the box):
rem   W A S D = move        Space = A (jump)     L = X (attack)
rem   Backspace = B         P = Y                E = right trigger (run)
rem   Enter = Start         Tab = Back           Shift+arrows = d-pad
setlocal
set "R=%~dp0"
set "BUILD=%R%out\build\win-amd64-release"

if not exist "%BUILD%\rayman.exe" (
    echo rayman.exe not found in %BUILD%. Build it first, see docs\WINDOWS.md.
    exit /b 1
)

"%BUILD%\rayman.exe" --game_data_root="%R%..\private\game" --gpu_plugin=xenos --mnk_mode=true %*
