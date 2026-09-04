#pragma once

// Fixed-size, same-user IPC between FrostBridgeNet and the injected game mod.
// The bridge can request only pause/resume; it cannot pass pointers or native calls.
#include <windows.h>
#include <string>

namespace frostsession {

constexpr DWORD magic = 0x31534246; // "FBS1"
constexpr DWORD version = 2;
constexpr LONG allowedStartSkewMs = 3000;

struct Control {
    DWORD signature = 0;
    DWORD layoutVersion = 0;
    volatile LONG modReady = 0;
    volatile LONG gameLoaded = 0;
    volatile LONG paused = 1;
    LONG reservedClock = 0;
    volatile LONG loadGeneration = 0;

    // Mod -> bridge: a player used Space or one of the native speed controls.
    volatile LONG localPauseSequence = 0;
    volatile LONG localPauseValue = 1;

    // Bridge -> mod: apply the peer/host pause state inside this game process.
    volatile LONG pauseCommandSequence = 0;
    volatile LONG pauseCommandValue = 1;
    volatile LONG pauseResultSequence = 0;
    alignas(8) volatile LONG64 gameTimeMs = 0; // actual calendar milliseconds, not local uptime
};

static_assert(sizeof(Control) == 56);

inline std::wstring name(DWORD pid) {
    return L"Local\\FrostBridgeSessionV2-" + std::to_wstring(pid);
}

} // namespace frostsession
