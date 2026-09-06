# Launch difficulty synchronization (2026-09-06)

The start handshake previously transmitted only the map index. Each client
therefore consumed its local difficulty sliders when the native start callback
ran, including Easy when the host selected Extreme.

Verified by read-only disassembly of the supported running executable:
scenario Start calls RVA 1A7CBF0, which calls ApplyDifficulty at 1A78BA0.
ApplyDifficulty uses panel+1C8/count+1D0, with 16-byte entries. The second
pointer is a slider whose selection is at +314. The byte at panel+1E0
selects the survivor branch. Endless uses panel+1B0/count+1B8 instead.

Launch IPC v5 and network protocol v16 carry bounded slider indices and the
survivor flag. Host capture occurs at the native Start click. Client preparation
applies and reads back the selections; commit reapplies the captured selection
immediately before the native callback. Mismatched settings abort the handshake.
Both peers must update. Existing city/save difficulty is not changed.

Validation: native-layout fixture tests for Extreme, mixed difficulty, invalid
values/counts; two real bridge processes with synthetic game mailboxes tested
for both story and endless handshakes, including propagation of Extreme.
DLL and bridge release builds passed. Actual new-city Steam/Sandboxie playtest
is still required after both game processes restart.
