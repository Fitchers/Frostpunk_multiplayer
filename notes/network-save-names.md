# Native multiplayer save names

The native save request now receives the normalized public slot, e.g.
`333_multiplayer`, not `fb_<checkpoint>_<player>_multiplayer`.
This changes future saves; old archives are not renamed or erased.

Each player retains an independent archive under
`Default/saves/FrostBridge/<player-key>/<slot>/<slot>.save`.
An exclusive per-profile/per-slot mutex serializes native writing and loading
for two test instances on one PC. The shared native slot is only a visible
working copy; the coordinator loads each player's validated private archive.
An ownership record protects unrelated native saves from replacement. Replaced
working copies are moved to `FrostBridge/native-backups`, not deleted.

Tests assert the exact native command names and file names, independent city
contents, consecutive `777` then `333` requests, no silent overwrite, and
automatic loading / missing-peer rejection. Native menu rendering is a separate
live check and is not established by these simulator assertions.
