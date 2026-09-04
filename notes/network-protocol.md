# FrostBridge network prototype

The protocol is transport-independent. Version 4 is carried either by Steam P2P
or by a length-prefixed direct TCP connection. TCP hosting listens on a selected
port; clients can use `127.0.0.1` for two local instances or the host's LAN IPv4.

## Identity

`480` is the Steam **AppID** used only for local development. It is not a user ID.
Each peer is addressed by its 17-digit `SteamID64`.

The current prototype uses Steam's legacy `ISteamNetworking` P2P interface because
it can be loaded through the flat C exports without distributing Valve's SDK
headers. The transport is isolated behind `FrostBridgeNet`, so it can be replaced
with `ISteamNetworkingMessages` after the project has its own Steamworks AppID and
official SDK integration.

## Packet framing

Steam packets use channel `17`; both transports start with a packed 32-byte header:

- magic: `FBP1`;
- protocol version: `4` (both peers must update; v3 packets are rejected);
- message type;
- monotonically increasing sender sequence;
- payload size;
- sender `SteamID64`;
- Unix timestamp in milliseconds.

Implemented messages:

- `Hello` / `HelloAck`: P2P handshake, required player name and city name;
- `Heartbeat`: keeps the implicit P2P session alive;
- `CitySnapshot`: eight packed int32 fields (32 bytes), in order: coal, wood,
  steel, steam cores, raw food, food rations, population, temperature;
- `Chat`: UTF-8 diagnostic text.
- `StartPrepare` (6), `StartReady` (7), `StartCommit` (8), `StartResult` (9):
  LAN-only start protocol; payload is uint32 request ID and int32 status.

Steam players currently specify each other's `SteamID64`. Each side sends first,
which implicitly accepts Steam's P2P session. Received packets are also restricted
to the configured peer and validated by magic, protocol version and payload size.

With `--watch-frostpunk`, the bridge validates the recovered `FrostpunkEconomy`
vtable and reads coal, wood, steel, steam cores, raw food and food rations from its own local process once
per second. A packet is sent every second even when values have not changed. Population
and temperature stay marked unavailable until their owning game structures are
recovered.

For direct LAN, one peer hosts and one joins. TCP frames contain a network-order
32-bit packet length followed by the same FrostBridge packet used by Steam.

When two Frostpunk processes run on one PC, `--pid` binds each bridge to the
correct local city.

## LAN start implementation and verification status

The host's form enables «Начать игру» after receiving a peer identity. The
console equivalent is `start`. The client cannot initiate this operation.
Prepare/ready checks both games before commit. A 10-second handshake deadline,
request IDs and per-process native launch state prevent duplicate launches.
Transport loss during commit can still result in only one game starting; this
is not a distributed atomic transaction. Errors are surfaced in the form log.

Each bridge accesses only its own game's `Local\FrostBridgeLaunchV1-PID` mapping.
The mapping contains a signature and a state, never addresses or arbitrary calls.
The DLL handles requests on the SDL window thread through WH_GETMESSAGE and
advances native menu callbacks on separate ticks. It selects Endurance, then
the first map with an enabled native Start button. DLC checks are preserved.
Other settings retain native defaults. Native callbacks are version-locked.

`dispatched` means the native Start callback was invoked, NOT that a city is
loaded. Actual resource reports require a readable economy in the local game.
One automatic launch per game process is supported in this prototype.

Both local and received reports use the player's handshake name:
`Анна: ресурсы: уголь 50; древесина 30; сталь 20; паровые ядра 3; сырая еда 80; пищевые пайки 0`.
This is the existing external connection-window chat, not an in-game overlay.
Cities remain independent; this does not synchronize buildings or implement trade.

Validation on 2026-09-04:

- C++ builds and MenuRoutingTests passed.
- Test-LanStart.ps1 passed against synthetic per-PID mappings: host authority,
  unavailable-client rejection, retry, two commit requests, duplicate prevention
  and Cyrillic resource labels. It does not launch live games.
- Live native mode selection and config callback execution were observed.
  The first map was unowned Rifts DLC; the native game correctly refused it.
- Source now advances through native map selection until Start is enabled.
  This final change is NOT verified in a live game: Windows Defender quarantined
  the updated DLL before deployment, and also quarantined FrostConnectionUI.exe.
  No security settings were changed. Successful two-city loading and live
  one-second resource cadence still require verification after this blocker.

### Resource identity fix and live verification (2026-09-04, 08:45)

The user's two-city screenshots confirmed that loading works, but exposed
incorrect resource identities. The early fixed indices varied per city/process.
`ResourceReader.h` now follows each record's resource-entry pointer and verifies
both internal Name and telemetry key; it never guesses by quantity or array order.
Unknown/unavailable/ambiguous snapshots are not transmitted as zeros.

Verified read-only in the loaded games PID 36096 (Fitchers) and PID 38208 (User):
both held coal 50, wood 30, steel 20, steam cores 3, in different record orders.
After updating only FrostBridgeNet and reconnecting LAN, BOTH GUI logs showed
correct named local and remote reports, repeating every second. The game
processes were not restarted and their memory/save files were not modified.
That identity-only fix retained v3. The subsequent food extension uses v4 and
adds raw food and food rations before population/temperature. Both peers must
update; v3 handshakes and snapshots are rejected instead of misinterpreted.

`tests/Test-ResourceReader.ps1` runs permutation, identity, corruption and city
reload regressions. `Test-LanStart.ps1` passed again against the staged bridge.
The previous quarantine/verification notes above describe the earlier state,
not a current deployment blocker.

## Next integration slice

1. Move this transport into a DLL-safe library.
2. Read the six snapshot fields from each local Frostpunk city.
3. Send snapshots periodically and expose the remote city in the Multiplayer UI.
4. Add explicit, validated gameplay commands such as resource shipment and shared
   events. The remote process remains authoritative for its own city.

## Connection form and lifecycle

Update 2026-09-04 08:24: after the user changed Defender's handling of the
file, the restored combined bridge ran successfully. Test-LanStart passed.
The updated menu DLL was built and deployed, and both games were restarted
with the user's approval. Native LAN clicks opened PID-bound bridge windows
for both games (42024 and 61948); the missing FrostConnectionUI.exe dependency
is resolved. Both native Start callbacks were subsequently dispatched, but
successful city loading is not yet verified. Earlier blocked results below
describe the preceding attempts, not the current LAN window status.

The menu DLL starts `FrostBridgeNet.exe --ui --lan|--steam --pid PID`.
The connection window and networking now live in ONE process. There is no
separate FrostConnectionUI executable, child bridge, pipe, Windows job or
TerminateProcess path in the form. `ConnectionSession.h` exposes typed options,
queued commands and bounded output messages to the UI.

Networking runs on an owned thread. Stop signals cancellation, wakes the session
and joins it. LAN accept polls cancellation every 25 ms; nonblocking connect has
a 5-second deadline per address. The GUI accepts numeric IPv4 addresses, avoiding
an uncancellable DNS lookup. CLI hostname support remains. Send has a 2-second
deadline. A mutex prevents duplicate windows for a city PID. Game handles are
read-only/synchronize; closing the form never terminates Frostpunk.

The combined executable compiled successfully on 2026-09-04, but Defender
quarantined it as Trojan:Win32/Wacatac.C!ml when Test-LanStart attempted to run.
Runtime tests of this refactor remain BLOCKED, not passed. The old DLL remains
loaded in the existing game and still refers to the removed standalone UI.
No quarantine restoration, exclusions, protection changes, binary obfuscation
or signature-only rebuilds were performed for this redesign.

`tests/Test-LanConnectionUI.ps1 -FirstPid PID1 -SecondPid PID2` tests two actual
forms with distinct running Frostpunk processes: blank names, invalid port,
Cyrillic and quote escaping, handshake, both directions of chat, disconnect,
cancel while waiting, and refused connection. It closes only its test forms
and their network threads. It does not modify cities or close games.
