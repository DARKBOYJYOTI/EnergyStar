@echo off
"%~dp0EnergyStar.Service.exe" --uninstall
if errorlevel 1 (
    echo EnergyStar service removal failed. Run this file as administrator.
    pause
    exit /b 1
)
echo EnergyStar service removed.
pause
