#pragma once
// Same-user, per-game IPC. No pointers or arbitrary native calls cross this boundary.
#include <windows.h>
#include <string>
namespace frostlaunch {
constexpr DWORD magic = 0x324C4246;
constexpr DWORD version = 5;
struct Difficulty {
    LONG count = 0;
    LONG survivor = 0;
    LONG levels[16]{};
    bool valid() const {
        if (count < 1 || count > 16 || survivor < 0 || survivor > 1) return false;
        for (LONG i = 0; i < count; ++i)
            if (levels[i] < 0 || levels[i] > 3) return false;
        return true;
    }
    bool operator==(const Difficulty&) const = default;
};
constexpr LONG chooseStory = -2;
constexpr LONG storyBase = 1000;
inline bool isStory(LONG index) { return index == chooseStory || (index >= storyBase && index < storyBase + 64); }
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
    Difficulty difficulty;
};
static_assert(sizeof(Control) == 88);
inline std::wstring name(DWORD pid) {
    return L"Local\\FrostBridgeLaunchV5-" + std::to_wstring(pid);
}
}
