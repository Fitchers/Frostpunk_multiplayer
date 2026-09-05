#pragma once

// Fixed-size, same-user IPC between FrostBridgeNet and the injected game mod.
// The bridge can request only pause/resume; it cannot pass pointers or native calls.
#include <windows.h>
#include <string>

namespace frostsession {

constexpr DWORD magic = 0x31534246; // "FBS1"
constexpr DWORD version = 4;
constexpr LONG allowedStartSkewMs = 3000;

struct Control {
    DWORD signature = 0;
    DWORD layoutVersion = 0;
    volatile LONG modReady = 0;
    volatile LONG gameLoaded = 0;
    volatile LONG paused = 1;
    LONG reservedClock = 0;
    volatile LONG loadGeneration = 0;

    // Mod -> bridge: any locally owned timer hold, excluding the network token.
    volatile LONG localPauseSequence = 0;
    volatile LONG localPauseValue = 1;

    // Bridge -> mod: apply the peer/host pause state inside this game process.
    volatile LONG pauseCommandSequence = 0;
    volatile LONG pauseCommandValue = 1;
    volatile LONG pauseResultSequence = 0;
    alignas(8) volatile LONG64 gameTimeMs = 0; // actual calendar milliseconds, not local uptime
    volatile LONG localSpeedSequence = 0;
    volatile LONG localSpeedValue = -1; // native modes 0, 1, 2; -1 unavailable
    volatile LONG speedCommandSequence = 0;
    volatile LONG speedCommandValue = -1;
    volatile LONG speedResultSequence = 0;
    volatile LONG currentSpeed = -1;
};

static_assert(sizeof(Control) == 80);

inline std::wstring name(DWORD pid) {
    return L"Local\\FrostBridgeSessionV4-" + std::to_wstring(pid);
}

} // namespace frostsession
