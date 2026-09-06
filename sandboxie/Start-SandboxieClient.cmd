@echo off
setlocal

set "SBIE=C:\Program Files\Sandboxie-Plus\Start.exe"
set "STEAM=D:\steam\steam.exe"
set "GAME=D:\Frostpunk\Frostpunk.exe"
set "LAUNCHER=C:\Users\serge\Desktop\My work\Frostpunk-RE-Lab\bin\FrostpunkMultiplayerLauncher.exe"

if not exist "%SBIE%" goto missing_sbie
if not exist "%STEAM%" goto missing_steam
if not exist "%GAME%" goto missing_game
if not exist "%LAUNCHER%" goto missing_launcher

echo Starting the persistent Steam test client in Sandboxie...
start "Sandboxed Steam" "%SBIE%" /box:FrostpunkSteamTest "%STEAM%"
echo.
echo Sign in to the SECOND Steam account in the yellow-bordered Steam window.
echo Do not use the account that is running outside Sandboxie.
echo When Steam is fully online, return here and press any key.
pause >nul

echo Starting the Frostpunk multiplayer client in the same sandbox...
"%SBIE%" /box:FrostpunkSteamTest /env:FROSTPUNK_EXE=%GAME% "%LAUNCHER%"
if errorlevel 1 goto launch_failed
exit /b 0

:missing_sbie
echo Sandboxie-Plus is missing: %SBIE%
goto failed
:missing_steam
echo Steam is missing: %STEAM%
goto failed
:missing_game
echo Frostpunk is missing: %GAME%
goto failed
:missing_launcher
echo Multiplayer launcher is missing: %LAUNCHER%
goto failed
:launch_failed
echo Sandboxie could not start the multiplayer launcher.
:failed
pause
exit /b 1
