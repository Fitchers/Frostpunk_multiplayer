# 🚨 Frostpunk Multiplayer — Steam P2P Community Test 🚨

Frostpunk Multiplayer lets each player run an independent city in the same
session. The host chooses the map, scenario and difficulty; the client receives
the same launch settings automatically.

## Quick start

1. Install Frostpunk 1.6.1 and Steam.
2. Extract the release folder and keep every included file together.
3. Run `FrostpunkMultiplayerLauncher.exe` for each player.
4. In Frostpunk, open **Multiplayer → Steam**.
5. Enter the other player's 17-digit SteamID64 on both sides and click
   **Connect**. The first click determines the host.
6. The host selects **Endless** or **Story Scenario**, chooses a map and sets
   the difficulty. The client starts the same selection automatically.

## In-game tools

- **TRADE** shows both cities and sends selected resources.
- **CHAT/SAVE** provides chat, synchronized multiplayer saves and loading.
- Pause and game speed are shared between connected players.
- Each player keeps control of their own city.

## Saving

If the host enters `survival`, the synchronized file is named
`survival_multiplayer`. Every player must have the matching save available when
continuing the session.

## Same-PC testing

Use two different Steam accounts. The repository includes `sandboxie/` helpers
for Sandboxie-Plus. The second Steam profile is stored persistently in its own
sandbox; the Frostpunk installation itself is reused from its normal location.

## Compatibility

The supported executable is Frostpunk 1.6.1 with SHA-256
`719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4`.
This is a two-player community test build. Frostpunk and Steam are not included.
