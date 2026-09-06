@echo off
setlocal

set "SBIE=C:\Program Files\Sandboxie-Plus\Start.exe"
set "GAME=D:\Frostpunk\Frostpunk.exe"
set "LAUNCHER=C:\Users\serge\Desktop\My work\Frostpunk-RE-Lab\bin\FrostpunkMultiplayerLauncher.exe"

if not exist "%SBIE%" goto missing
if not exist "%GAME%" goto missing
if not exist "%LAUNCHER%" goto missing

"%SBIE%" /box:FrostpunkSteamTest /env:FROSTPUNK_EXE=%GAME% "%LAUNCHER%"
if errorlevel 1 pause
exit /b %errorlevel%

:missing
echo A required file is missing. Run Start-SandboxieClient.cmd to see which one.
pause
exit /b 1
