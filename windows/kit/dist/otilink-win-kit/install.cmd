@echo off
setlocal
cd /d "%~dp0"
if /i "%~1"=="quiet" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -QuietOnly
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
)
echo.
pause
