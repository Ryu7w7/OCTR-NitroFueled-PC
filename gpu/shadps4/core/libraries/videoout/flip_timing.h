// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <chrono>

namespace Libraries::VideoOut {
inline auto NextFlipDeadline(std::chrono::steady_clock::time_point previous,
                             std::chrono::steady_clock::time_point now,
                             std::chrono::nanoseconds period) {
    // Discard missed slots after loading or a slow frame instead of presenting a burst.
    return std::max(previous, now) + period;
}
} // namespace Libraries::VideoOut
