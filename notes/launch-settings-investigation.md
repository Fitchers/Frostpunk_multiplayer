# Launch settings and reload overlay investigation (2026-09-05)

Status: overlay changes built/deployed; live save-load verification pending.
Temperature synchronization and story launch are NOT implemented yet.

## Overlay/reload

The overlay previously received its only refresh messages from workerThread,
which also performs locateLivePanel scans. It now owns 100 ms window timers,
and the button sends a visibility refresh immediately on click. The real
window-procedure test checks immediate collapse without pumping worker/timer
messages. This does not by itself prove the reported live latency is gone.

Resource objects can exist before a native save load finishes. The named
IsLoadingScreenActive predicate at RVA F283C0 reads the byte at RVA 2A602DD.
Session readiness now also requires this byte to be zero. Further verify that
SaveSync retains its shared-slot lock throughout the entire native load.

## Native startup investigation (read-only disassembly)

Supported executable/build remains the one documented in the project.
LaunchControl V3 currently carries only mapIndex. Difficulty, mode options,
and random/weather state are not transferred. A different temperature is not
yet proven to be caused by difficulty rather than randomized weather.

Endless Start: RVA 1A8ADE0. Native implementation reads map selection at
panel+110 and definition at panel+F0; builds a 0xF0-byte launch configuration.
At 1A8B08D it iterates panel+1B0 (count at +1B8), 16-byte entries; reads a
selection index at entry's second pointer+314 and copies difficulty entries
into the launch configuration. Additional mode toggles are iterated through
panel+1C8, count+1D0. Do not copy raw pointers between processes.

Scenario Start: RVA 1A7BEA0 (already intercepted for transport selection).
Selected scenario index is panel+1B0, count+1A0, pointer array+198.
Native callback performs entitlement/progression checks before dispatching
to RVA 1A7CBF0 (panel, scenario definition); preserve these checks. Merely
displaying a Campaign selector without a new synchronized launch path is
not a completed feature.

Next: recover portable scenario/settings identities and native setters;
extend/version IPC and network start payload with validated settings, then
test both Endless and an available story scenario on two independent cities.
For existing saves, retain each city's own state; do not replace the client's
save with the host's to make temperatures appear equal.
