# Recovered Frostpunk economy API

These findings apply only to the executable with SHA-256:

```text
719c6e016bcdb1021a6401b1624aebdcfb03c6782a763d2fbd8c2805f9dae9d4
```

Absolute heap and module addresses below are from one process launch and must not
be reused. Static RVAs are relative to the loaded `Frostpunk.exe` base.

## Confirmed class and globals

MSVC RTTI for the object captured in `RCX` resolves to:

```text
.?AVFrostpunkEconomy@@  ->  class FrostpunkEconomy
```

| Item | RVA/offset | Evidence |
|---|---:|---|
| `gEconomy` pointer | `Frostpunk.exe + 0x3FC93B0` | Runtime qword matched the captured `this` pointer |
| `FrostpunkEconomy` vtable | `Frostpunk.exe + 0x1FA66A0` | MSVC Complete Object Locator and TypeDescriptor |
| Resource container | `FrostpunkEconomy + 0x2240` | Base pointer followed by count/capacity |
| Container count | `FrostpunkEconomy + 0x2248` | Runtime value `11` |
| Container capacity | `FrostpunkEconomy + 0x224C` | Runtime value `12` |

## Resource record

The resource container is an array with a stride of `0x70` bytes:

```cpp
struct FrostpunkEconomyResourceRecord {
    FrostpunkResourceEntry* resource; // +0x00
    std::int32_t amount;              // +0x08
    std::int32_t capacity;            // +0x0C
    // unknown fields                 // +0x10 .. +0x6F
}; // sizeof == 0x70
```

Observed indices in the current scenario:

| Index | Observed amount | Capacity | Identification |
|---:|---:|---:|---|
| 0 | 30 | 300 | Wood |
| 4 | 3 | 300 | Steam cores |
| 7 | 41 | 900 | Coal |
| 8 | 20 | 300 | Steel |
| 9 | 80 | 300 | likely raw food |

Index identities should eventually be verified using each
`FrostpunkResourceEntry` rather than assumed globally.

## Resource mutation function

The core mutation routine begins at:

```text
Frostpunk.exe + 0x168E340
```

Provisional signature:

```cpp
bool FrostpunkEconomy_ChangeResource(
    FrostpunkEconomy* economy,       // RCX
    FrostpunkResourceEntry* resource,// RDX
    std::int32_t requestedDelta,     // R8D
    void* eventContext,              // R9
    std::int32_t changeType,         // stack argument; exact enum unknown
    bool option);                    // stack argument; meaning unknown
```

The last two parameter names are intentionally provisional. The routine locates
the resource in the `0x70`-stride container, clamps the requested change, updates
the amount and dispatches listeners/events.

The actual write is at RVA `0x168E438`:

```asm
imul rax, rcx, 70h
...
add  dword ptr [rax + rbp + 8], r15d
```

Captured runtime values for generator consumption:

```text
requested/clamped delta: -1
old coal:                42
new coal:                41
watched entry index:     7
```

The function at RVA `0x168E940` is a strong match for a public/integer-multiplier
`FrostpunkEconomy::ChangeResources` overload. A caller at RVA `0x18315CA` invokes
it and then checks the assertion string:

```text
gEconomy->ChangeResources should succeed.
```

Other direct calls to the core routine occur at RVAs:

```text
0x168E641  0x168E675  0x168E691  0x168E8EC  0x168EA1C
0x168EB4A  0x168EC8B  0x169119E  0x16911CD  0x1691275
```

## Next controlled experiment

Use a hardware execute breakpoint at `Frostpunk.exe + 0x168E340`. When the call's
`RDX` equals the coal `FrostpunkResourceEntry*` and `R8D == -1`, replace `R8D`
with `+100` once, then detach. This executes the game's own resource-change path
on its simulation thread and preserves its normal notifications. It is a cleaner
signature test than directly overwriting the resource field.

## Controlled mutation result

The experiment was completed successfully. With coal at `41`, an execute
breakpoint intercepted the next call for the coal resource with `R8D == -1`.
FrostProbe changed `R8D` to `+159` exactly once. The game's own routine then
updated coal from `41` to `200`, and the in-game interface displayed `200`.

This confirms all of the following together:

- RVA `0x168E340` is on the live resource mutation path;
- `RDX` identifies the target `FrostpunkResourceEntry`;
- `R8D` carries the requested signed resource delta;
- the recovered `gEconomy`, container and coal-record layout are consistent;
- using the engine path updates the visible game state without a direct field write.
