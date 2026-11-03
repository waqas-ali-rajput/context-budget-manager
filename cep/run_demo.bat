@echo off
rem Double-click to open the interactive CEP demo. Builds cbm.exe first if it is missing.
cd /d "%~dp0"
if not exist cbm.exe call .\build.bat
if not exist cbm.exe (
  echo Build failed: install g++ ^(MinGW / TDM-GCC^) and add it to PATH.
  pause
  exit /b 1
)
.\cbm.exe menu
pause
