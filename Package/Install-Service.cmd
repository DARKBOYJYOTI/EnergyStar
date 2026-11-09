@echo off
"%~dp0EnergyStar.Service.exe" --install
if errorlevel 1 (
    echo EnergyStar service installation failed. Run this file as administrator.
    pause
    exit /b 1
)
echo EnergyStar service installed and started.
pause
