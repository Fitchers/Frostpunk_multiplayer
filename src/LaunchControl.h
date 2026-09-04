#pragma once
// Same-user, per-game IPC. No pointers or arbitrary native calls cross this boundary.
#include <windows.h>
#include <string>
namespace frostlaunch {
constexpr DWORD magic = 0x314C4246;
enum State : LONG { unavailable=0, ready=1, requested=2, dispatched=3, failed=-1 };
struct Control { DWORD signature; volatile LONG state; };
inline std::wstring name(DWORD pid) {
    return L"Local\\FrostBridgeLaunchV1-" + std::to_wstring(pid);
}
}
