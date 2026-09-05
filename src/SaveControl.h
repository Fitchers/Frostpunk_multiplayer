#pragma once
#include <windows.h>
#include <string>
#include <optional>
#include <algorithm>
#include <cwctype>

namespace frostsave {
constexpr DWORD magic = 0x31534246; // independent named mapping
constexpr DWORD version = 1;
constexpr size_t nameCapacity = 96;
enum Operation : LONG { none = 0, save = 1, load = 2 };

// A slot is a display name, never a path supplied by a network peer.
inline std::optional<std::wstring> slotName(std::wstring value) {
    while (!value.empty() && value.front() == L' ') value.erase(value.begin());
    while (!value.empty() && value.back() == L' ') value.pop_back();
    if (value.empty() || value.back() == L'.') return {};
    for (wchar_t ch : value)
        if (ch < 32 || ch == 127 || std::wstring(L"<>:\"/\\|?*").find(ch) != std::wstring::npos)
            return {};
    // Reject malformed UTF-16 rather than let peers disagree on filenames.
    for (size_t i=0; i<value.size(); ++i) {
        auto ch=static_cast<unsigned>(value[i]);
        if(ch>=0xD800 && ch<=0xDBFF) {
            if(++i>=value.size() || value[i]<0xDC00 || value[i]>0xDFFF) return {};
        } else if(ch>=0xDC00 && ch<=0xDFFF) return {};
    }
    std::wstring lower=value;
    std::transform(lower.begin(),lower.end(),lower.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
    const std::wstring suffix=L"_multiplayer";
    if(lower.size()>=suffix.size() && lower.compare(lower.size()-suffix.size(),suffix.size(),suffix)==0)
        value.replace(value.size()-suffix.size(),suffix.size(),suffix);
    else value+=suffix;
    if(value.size()>=nameCapacity) return {};
    return value;
}

struct Control {
    DWORD signature = 0;
    DWORD layoutVersion = 0;
    volatile LONG modReady = 0;
    volatile LONG role = 0; // 0 offline, 1 host, 2 client
    volatile LONG eventSequence = 0;
    LONG eventOperation = 0;
    wchar_t eventName[nameCapacity]{};
    volatile LONG commandSequence = 0;
    LONG commandOperation = 0;
    wchar_t commandName[nameCapacity]{};
    volatile LONG ackSequence = 0;
    volatile LONG result = 0; // 1 queued, -1 rejected; NOT disk completion
    volatile LONG64 heartbeat = 0; // bridge lease; hooks fail open if it exits/crashes
};
static_assert(sizeof(Control)==432);
inline std::wstring name(DWORD pid) { return L"Local\\FrostBridgeSaveV1-"+std::to_wstring(pid); }
}
