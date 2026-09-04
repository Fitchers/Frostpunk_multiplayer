@echo off
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -File ".\scripts\Start-FrostpunkMultiplayer.ps1"
if errorlevel 1 pause
