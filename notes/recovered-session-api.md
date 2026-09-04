# Native pause, game clock and city mood

Target: Frostpunk.exe SHA-256
`719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4`.
Read-only analysis of both live processes on 2026-09-04. Heap addresses are not reusable.

## Pause

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
