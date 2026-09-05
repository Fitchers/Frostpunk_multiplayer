# Native pause, game clock and city mood

Target: Frostpunk.exe SHA-256
`719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4`.
Read-only analysis of both live processes on 2026-09-04. Heap addresses are not reusable.

## Pause

### Shared speed (protocol 12, session IPC V4)

Native selector RVA `0x11BD080` takes the time-controller pointer (`0x2B6A710`,
vtable `0x1DFCB80`) and a mode index 0..2. It reads float multipliers at
`0x2B70CB4`: 1, 3, 12, and updates GameplayTimer `+0xD0` plus the native UI event.
These are the three stock speed buttons, not literal x1/x2/x3 multipliers.
The DLL validates the prologue, calls on the game window thread and acknowledges
only after reading back the requested mode. It never removes a pause token.
V4 is 80 bytes: speed event sequence/value at 56/60, command sequence/value at
64/68, acknowledgement at 72, actual current speed at 76. Event and current
values are separate so applying an inbound command cannot erase a local input.
The host serializes local choices and client requests into reliable, revisioned
speedState packets. The client ignores stale revisions; applied modes do not
generate new input events. The host publishes its initial mode after loading.
LAN simulator tests exercise all three modes, client override, invalid values,
simultaneous requests and lack of echo, alongside existing pause/drift tests.
Live V4 verification in PIDs 44104/54812 applied mode 1 and mode 2 from the host,
then mode 0 from the client. Both native current-speed fields matched after every
command and both acknowledgement sequences matched. A subsequent three-second
sample recorded zero new commands and zero new local events, confirming that the
asynchronous native event is suppressed rather than echoed back over the network.

### Update 2026-09-05: timer reasons and continuous correction

Anti-stutter follow-up (protocol 11): live IPC counted 20 and 19 pause-command
changes per two seconds in PIDs 11916/13072 with localPause=0. The old 3000 ms
threshold was below the observed 4800 ms calendar quantum. ClockCorrection now
ignores <=15000 ms drift, requires 750 ms of continuous excess drift, and releases
at <=5000 ms. User/UI and unloaded/stale-peer holds bypass that filter. This is
a deliberate accuracy/smoothness tradeoff, not a promise of a 3-second bound.
Tests cover 10000 frame-jitter samples, transient spikes, leader reversal,
user-pause interaction and real LAN IPC with zero jitter-induced commands.
The protocol-10 measurements below document the superseded controller.
After deploying protocol 11 to both bridges without restarting the games,
40 live samples at 250 ms intervals recorded zero pause-command changes in
each process, equal calendar advance of 1473600 ms, and 0–9600 ms skew.
Both local-pause flags remained zero. This verifies removal of pause chatter;
it is not an FPS benchmark or a strict bound on future network jitter.

Current implementation supersedes the UserPause-only prototype described below.
GameplayTimer pointer RVA `0x2B68510`, vtable `0x1D716F0`, reason array `+0xB8`,
count `+0xC0`. Native add/remove reason functions: `0xF6C530` / `0xF6CA80`.
Reason identity is a pointer. The mod owns one static network token and never
removes the engine's user/UI reasons. Only non-network reasons propagate to peers.
Session IPC V3 retains the 56-byte layout but changes pause-command semantics.

Protocol 10 continuously compares calendar times (25 ms network publication).
Only the leading simulation is held; the lagging city runs its ordinary updates.
Stale peer data (>2 seconds) or an unloaded peer holds the local city. Correction
starts above 3000 ms and releases below 500 ms. Native time is not rewritten.
This is approximate synchronization, not deterministic lockstep. In live PIDs
11916/13072, 20 samples over 5 seconds showed 0/4800/9600 ms skew while both
clocks progressed. The observed native calendar quantum was 4800 ms, so a hard
3000 ms bound is not established. Opening UI remains a separate shared pause.

### Map launch update

Launch IPC V3: mapIndex -1 waits for the host's real Start click. The hook at
`0x1A8ADE0` captures panel `+0x110` and defers the original function until the
client prepares that index. Client selection uses `0x1A8B880`; index readback
must match before commit. Stock entitlement checks remain inside native Start.
Prepared clients cannot independently launch while awaiting commit.
Success is now acknowledged after a city actually loads, not merely after the
callback returns. Both PIDs above confirmed mapIndex 1 and loaded cities after
the host selection at 02:42:24, with native callbacks 28 ms apart and load
confirmations at 02:42:27.731 / 02:42:27.873. No client Start was issued by the
agent. Mode is currently the first endless mode; difficulty/seed are not copied.

### Earlier UserPause-only prototype (superseded)

`BUTTON_PAUSE` string RVA 0x2147368 is bound at 0x1B1A51D to callback
0x1B1A8D0. That callback reads an event value -1 for pause (0/1/2 for speeds),
calls speed selector 0x11BD080 for nonnegative values, then calls
`SetUserPause(this, paused, false)` at **0x11BD0E0**.

The object pointer is at **0x2B6A710**, vtable **0x1DFCB80**.
RTTI property registration at 0x11BCB4E names **UserPause**, offset **0xE0**.
The setter checks that byte, updates timer pause reasons and publishes the native
change event. Its third argument can override a native freeze restriction; we
pass false, matching the stock button and respecting the restriction.

The mod invokes the setter on the existing game-window-thread message hook,
checks its prologue and vtable, then reads UserPause back. An IPC acknowledgement
is published only when readback matches the requested state. Failed/loading
requests retry. Native flag changes not caused by our call propagate as player
pause events. No guessed mouse coordinates, Space toggles or optimistic flags.

Calendar conversion at **0x11AE360** is called by the native time panel with
`object + 0xB0`. It divides the signed 64-bit tick count by
`(int32 at module + 0x2B70CC0) << 31` for hours, then computes minutes/seconds.
The observed scale is 25. Session IPC v2 publishes 64-bit **calendar milliseconds**
from that timer; it no longer accumulates wall time since a mod joined.

The network start barrier now holds a loaded local city before checking whether
the peer is loaded. Release requires both native pause flags and <=3000 calendar
milliseconds skew. A larger skew remains paused and is reported; automatic
catch-up/time rewrites are not implemented by this change.

Native invocation/end-to-end pause in actual Frostpunk still requires restarting
the games with the new DLL and checking that the HUD clock stops. Passing the
IPC simulator tests alone does not establish that live result.

## Hope / discontent

| Field | Hope system | Discontent system |
|---|---:|---:|
| Global pointer RVA | 0x3FD3DE0 | 0x3FD1410 |
| Vtable RVA | 0x2013278 | 0x1FF0140 |
| Initialized byte | 0x2160 | 0x2198 |
| Enabled byte | 0x2161 | 0x2199 |
| Total float | 0x2164 | 0x21E8 |
| Citizen count int32 | 0x2168 | 0x21EC |
| Native average getter RVA | 0x1827450 | 0x17A88A0 |

Offsets are confirmed by named RTTI registration and the getters used by the
native HUD at 0x1B07D0D and 0x1B078B8. Both compute total/max(count,1).
`CityVitals.h` validates object identity, enabled/initialized state, bounded count
and finite total, then returns clamped hundredths of a percent; -1 is unavailable.

Read-only integration: PID 59680 hope 40%, discontent 0.81%; PID 2220 hope 40%,
discontent 17.85%. These are independent cities, not copied values. No mood writes.

## Overlay / transfer verification

`Test-OverlayControls.ps1` exercises the actual native edit controls, sliders and
window procedure in a separate test process, without injecting into a game. It
covers 1-core selection, field/slider updates, local-stock return, peer-empty
eligibility, invalid amounts, busy state and the published outgoing amount.
It saves an off-screen GDI rendering to `artifacts/overlay-controls/preview.bmp`.
`Test-LanTrade.ps1` verifies 1/17/50 transfers and returns in both directions,
invalid requests and partial debit refunds. `Test-SessionSync.ps1` specifically
starts with a running host and an unloaded client to catch the old barrier bug.
