#pragma once
// Same-user, per-game IPC. No pointers or arbitrary native calls cross this boundary.
#include <windows.h>
#include <string>
namespace frostlaunch {
constexpr DWORD magic = 0x324C4246;
constexpr DWORD version = 3;
enum State : LONG {
    unavailable=0,
    ready=1,
    prepareRequested=2,
    prepared=3,
    commitRequested=4,
    dispatched=5,
    failed=-1
};
struct Control {
    DWORD signature;
    DWORD layoutVersion;
    volatile LONG state;
    volatile LONG mapIndex; // -1 waits for the host's native Start click.
};
static_assert(sizeof(Control) == 16);
inline std::wstring name(DWORD pid) {
    return L"Local\\FrostBridgeLaunchV3-" + std::to_wstring(pid);
}
}
