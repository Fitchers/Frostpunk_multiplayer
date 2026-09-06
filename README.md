# 🚨 Frostpunk Multiplayer — Steam P2P Community Test 🚨

**Multiplayer experiences for single-player games.**

This project adds a Steam P2P multiplayer layer to Frostpunk. Each player
controls an independent city. The host selects the map, scenario and difficulty;
the client receives the same selection and starts automatically.

> This is an experimental two-player community test build. Frostpunk and Steam
> are not included.

## Download and install

Download the latest ZIP from the repository's Releases page, extract it as one
folder, and keep the included EXE and DLL files together. Run
`FrostpunkMultiplayerLauncher.exe`; it starts Frostpunk and loads the mod.

Do not copy the mod files into the Frostpunk installation directory.

## Connect through Steam

1. Both players run the launcher and open **Multiplayer → Steam** in Frostpunk.
2. Each enters the other player's 17-digit SteamID64.
3. Both click **Connect**. The first player to click becomes the host.
4. The host chooses **Endless** or **Story Scenario**, a map and a difficulty.
5. The client receives the same map and difficulty and starts automatically.

Inside the game, **TRADE** handles resource transfers and **CHAT/SAVE** handles
chat and synchronized multiplayer saves. Pause and game speed are shared.

## Compatibility

The supported game is Frostpunk 1.6.1 with SHA-256
`719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4`.
Every player needs a separate Steam account and a separate Frostpunk install.
Steam AppID 480 is used for the test transport.

For the complete player guide, see [`docs/PLAYER-GUIDE.md`](docs/PLAYER-GUIDE.md).
For the downloaded package, see [`packaging/README.txt`](packaging/README.txt).

## Development

The project is tied to the verified executable build above. Reverse-engineering
notes, protocol details and native layout research are kept under `notes/`.
Build scripts are under `scripts/`. The project does not ship Frostpunk assets,
Steam SDK files or game data.
