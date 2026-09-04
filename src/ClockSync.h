#pragma once
#include <cstdint>

namespace frostsync {
// Independent of user/UI pause. Never rewinds the simulation or edits its clock.
// The native calendar advances in 4800 ms steps in the supported live build.
// Reacting below one frame made both games alternately pause about 10 times/sec.
class ClockCorrection {
public:
    static constexpr std::int64_t enterSkewMs = 15000;
    static constexpr std::int64_t releaseSkewMs = 5000;
    static constexpr std::int64_t confirmationMs = 750; // monotonic wall time

    bool update(std::int64_t local, std::int64_t peer, std::int64_t now,
                bool userPaused = false) {
        const auto ahead = local > peer ? local - peer : 0;
        if (held_) {
            if (ahead <= releaseSkewMs) reset();
            return held_;
        }
        if (userPaused || ahead <= enterSkewMs) { reset(); return false; }
        if (candidateSince_ < 0 || now < candidateSince_) candidateSince_ = now;
        if (now - candidateSince_ >= confirmationMs) held_ = true;
        return held_;
    }
    void reset() { held_ = false; candidateSince_ = -1; }
private:
    bool held_ = false;
    std::int64_t candidateSince_ = -1;
};
}
