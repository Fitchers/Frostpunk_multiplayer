#pragma once
#include <cstdint>

namespace frostsync {
// Independent of user/UI pause. Never rewinds the simulation or edits its clock.
inline bool holdAhead(std::int64_t local, std::int64_t peer, bool held) {
    return local > peer && local - peer > (held ? 500 : 3000);
}
}
