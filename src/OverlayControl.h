#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace frostoverlay {

constexpr DWORD magic = 0x324F4246; // "FBO2"
constexpr DWORD version = 3;
constexpr LONG maxTransferAmount = 1000000;
inline bool validAmount(LONG amount) { return amount > 0 && amount <= maxTransferAmount; }
constexpr std::size_t resourceCount = 6;

enum class Resource : LONG { coal, wood, steel, steamCores, rawFood, foodRations };
enum class Connection : LONG { offline, connected };

struct Values {
    LONG item[resourceCount]{};
};

struct Snapshot {
    volatile LONG sequence = 0; // odd while writing, even when stable
    Values values{};
};

struct Control {
    DWORD signature = 0;
    DWORD layoutVersion = 0;
    volatile LONG modReady = 0;
    volatile LONG connection = 0;
    volatile LONG namesSequence = 0;
    char localName[64]{};
    char peerName[64]{};
    Snapshot local{};
    Snapshot peer{};

    // Overlay -> bridge. Payload is written first, sequence published last.
    LONG outgoingResource = 0;
    LONG outgoingAmount = 0;
    volatile LONG outgoingSequence = 0;
    volatile LONG transferBusy = 0;

    // Bridge -> injected mod -> bridge. Only the game executes this request.
    LONG applyResource = 0;
    LONG applyDelta = 0;
    volatile LONG applyRequestSequence = 0;
    LONG applyResult = 0;
    volatile LONG applyResultSequence = 0;

    volatile LONG notificationSequence = 0;
    wchar_t notification[160]{};
    volatile LONG localHope = -1;
    volatile LONG localDiscontent = -1;
    volatile LONG peerHope = -1;
    volatile LONG peerDiscontent = -1;
    // 0 unknown, 1 loading, 2 playing, 3 paused. Skew: local minus peer.
    volatile LONG localState = 0;
    volatile LONG peerState = 0;
    volatile LONG skewSeconds = 0;
    volatile LONG historySequence = 0;
    wchar_t history[3][160]{}; // newest first; guarded by odd/even sequence
};
static_assert(offsetof(Control, local) == 148);
static_assert(offsetof(Control, outgoingResource) == 204);
static_assert(offsetof(Control, applyResource) == 220);
static_assert(offsetof(Control, applyResultSequence) == 236);
static_assert(sizeof(Control) == 1556);

inline std::wstring name(DWORD pid) {
    return L"Local\\FrostBridgeOverlayV4-" + std::to_wstring(pid);
}

inline bool validResource(LONG value) {
    return value >= 0 && value < static_cast<LONG>(resourceCount);
}

inline void writeSnapshot(Snapshot& target, const Values& value) {
    InterlockedIncrement(&target.sequence);
    MemoryBarrier();
    target.values = value;
    MemoryBarrier();
    InterlockedIncrement(&target.sequence);
}

inline bool readSnapshot(const Snapshot& source, Values& value) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG before = InterlockedCompareExchange(
            const_cast<volatile LONG*>(&source.sequence), 0, 0);
        if (before & 1) continue;
        MemoryBarrier();
        value = source.values;
        MemoryBarrier();
        const LONG after = InterlockedCompareExchange(
            const_cast<volatile LONG*>(&source.sequence), 0, 0);
        if (before == after && !(after & 1)) return true;
    }
    return false;
}

} // namespace frostoverlay
