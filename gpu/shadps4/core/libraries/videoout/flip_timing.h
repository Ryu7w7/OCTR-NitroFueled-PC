// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <chrono>

namespace Libraries::VideoOut {
inline auto NextFlipDeadline(std::chrono::steady_clock::time_point previous,
                             std::chrono::steady_clock::time_point now,
                             std::chrono::nanoseconds period) {
    // On time or a little late (wake-up jitter, a vblank): the next slot of the same grid, so a limit
    // of 60 gives 60 frames a second, not 59 (each frame's lateness added to the next one's wait).
    // A whole slot or more missed (loading, a slow frame): those slots are dropped, no burst.
    const auto next = previous + period;
    return next > now ? next : now + period;
}
} // namespace Libraries::VideoOut
