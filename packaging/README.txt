FROSTPUNK MULTIPLAYER — TEST BUILD

1. Extract this entire folder. Keep all files together.
2. Start Steam and sign in.
3. Run FrostpunkMultiplayerLauncher.exe. It starts ONE game, or attaches to
   your already running Frostpunk. If needed, select your Frostpunk.exe once.
4. Both players open Multiplayer > Steam, enter each other's SteamID64 and
   click Connect. The player who clicks Connect first becomes the host;
   the selected role is shown explicitly in the connection panel.
5. The host chooses Endless or Story Scenario, then selects a map in game.
   Each player controls their own city. Use the Multiplayer resource panel
   after loading. The connection panel's Minimize button keeps the session.
6. For a direct LAN connection select LAN and enter the host's IPv4 address.

The connection panel appears over the game; networking runs in a background
helper that exits with the game. Do not run the helper manually.
No PowerShell installation or two-instance test launcher is required.

Current limitations:
- Two players. Steam uses test AppID 480; cross-account testing is still needed.
- Only the verified Frostpunk 1.6.1 executable is supported. The launcher checks
  SHA-256: 719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4.
  Other executables, including other store builds, need separate verification.
- Weather timelines are not synchronized yet.
- Story launch and the new connection panel need end-to-end testing.
- Use the same player name when resuming a multiplayer checkpoint.

This package contains the mod, not Frostpunk. Every player needs their own game.
This is a test package for friends, not a verified production release.
