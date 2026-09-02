# Frostpunk build 719c6e016bcd

## Identity

- Executable: `D:\Frostpunk\Frostpunk.exe`
- SHA-256: `719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4`
- Distribution indicators: `Galaxy64.dll` and `goggame-1648559910.*` (GOG build)
- Architecture: x64
- Preferred image base: `0x140000000`
- Entry point RVA: `0x1B8A860`
- Image size: `0x44DD000`
- PE sections: 7
- Linker timestamp: `2020-11-20 10:40:38 UTC` (metadata, not trusted as a release date)

## Debug and type information clues

The executable contains an RSDS CodeView record:

```text
E:\AgentSourceDir\FrostPunk\LiquidEngine\Final GOGX64\Frostpunk.pdb
GUID EB2CF40E-07D7-4BA2-AFD8-B93D6BC0395E, age 18
```

The PDB itself is not present, but source paths, RTTI names, assertions and logging
strings remain in the executable. Useful initial types include:

```text
FrostpunkResourceAmount
FrostpunkFloatResourceAmount
FrostpunkResourceEntry
FrostpunkChangeResourcesEffect
FrostpunkResourceAmountChangedEventTrigger
FrostpunkResourceExtractionSystem
FrostpunkResourceGatheringSystem
FrostpunkResourceDropOffSystem
```

Useful source-path strings include:

```text
liquidengine\frostpunk\frostpunkresourceamount.h
liquidengine\frostpunk\frostpunkresourcesourcesystem.cpp
liquidengine\frostpunk\frostpunkresourceextractionsystem.cpp
liquidengine\frostpunk\frostpunkresourcegatheringsystem.cpp
```

Resource identifiers visible as strings include `ResourceCoal`, `ResourceWood`,
`ResourceSteel` and `FrostpunkEconomy::Resource`.

## Packing hypothesis

This build does not currently look conventionally packed: it has ordinary `.text`,
`.rdata`, `.data`, `.pdata`, `.rsrc` and `.reloc` sections, a normal import table,
large amounts of readable RTTI/source-path text and a CodeView record. This is not
proof that no individual data is encoded, but it means static analysis can start
directly from this executable.

## First runtime attempt

- Frostpunk PID: `24936`
- Frostpunk integrity: High (`RID 12288`)
- Steam integrity: High (`RID 12288`)
- Codex/tool integrity: Medium (`RID 8192`)
- `PROCESS_VM_READ`: denied with Win32 error 5

Restart Steam normally at Medium integrity, then create a fresh scan state. ASLR
means absolute runtime addresses must never be copied between process launches;
store module-relative RVAs when code locations are identified.
