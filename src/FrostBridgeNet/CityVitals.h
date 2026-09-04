#pragma once
#include "ResourceReader.h"
#include <algorithm>
#include <cmath>

namespace frostbridge::vitals {
// Hundredths of a percent, -1 means unavailable/disabled. See recovered-session-api.md.
struct Snapshot { std::int32_t hope = -1, discontent = -1; };
template<class Reader>
std::int32_t average(Reader& read, std::uintptr_t base, std::uintptr_t global,
                     std::uintptr_t vtable, std::uintptr_t initialized,
                     std::uintptr_t totalOffset) {
    using resources::value;
    const auto object = value<std::uintptr_t>(read, base + global);
    if (!object || !resources::pointer(*object) ||
        value<std::uintptr_t>(read, *object) != base + vtable ||
        value<std::uint8_t>(read, *object + initialized) != 1 ||
        value<std::uint8_t>(read, *object + initialized + 1) != 1) return -1;
    const auto total = value<float>(read, *object + totalOffset);
    const auto count = value<std::int32_t>(read, *object + totalOffset + 4);
    if (!total || !count || !std::isfinite(*total) || *count < 0 || *count > 1000000 ||
        value<std::uintptr_t>(read, base + global) != object) return -1;
    return static_cast<std::int32_t>(std::lround(
        std::clamp(*total / static_cast<float>(std::max(1, *count)), 0.0f, 1.0f) * 10000.0f));
}
template<class Reader>
Snapshot readSnapshot(Reader read, std::uintptr_t base) {
    return {average(read, base, 0x3FD3DE0, 0x2013278, 0x2160, 0x2164),
            average(read, base, 0x3FD1410, 0x1FF0140, 0x2198, 0x21E8)};
}
}
