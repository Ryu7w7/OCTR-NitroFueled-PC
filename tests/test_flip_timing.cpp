// SPDX-License-Identifier: GPL-2.0-or-later
// Run: c++ -std=c++20 -I. tests/test_flip_timing.cpp -o /tmp/test_flip_timing && /tmp/test_flip_timing
#include <cassert>
#include "gpu/shadps4/core/libraries/videoout/flip_timing.h"

int main() {
    using namespace std::chrono;
    using Libraries::VideoOut::NextFlipDeadline;
    const steady_clock::time_point start{};
    const auto period = nanoseconds(1000000000 / 60);
    auto deadline = NextFlipDeadline(start, start, period);
    assert(deadline == start + period);
    for (int frame = 1; frame <= 120; ++frame) {
        deadline = NextFlipDeadline(deadline, start + frame * period, period);
        assert(deadline == start + (frame + 1) * period);
    }
    // A race restart or GPU stall must not leave any immediately eligible catch-up slots.
    for (auto gap : {milliseconds(20), milliseconds(500), milliseconds(10000)}) {
        const auto resumed = deadline + gap;
        deadline = NextFlipDeadline(deadline, resumed, period);
        assert(deadline == resumed + period);
        assert(NextFlipDeadline(deadline, deadline, period) == resumed + 2 * period);
    }
    assert(NextFlipDeadline(start, start + seconds(10), nanoseconds(0)) == start + seconds(10));
    // Flips a little late each time (a player's 60 FPS limit showed 59): no drift, still 60 a second.
    deadline = start;
    for (int frame = 1; frame <= 600; ++frame)
        deadline = NextFlipDeadline(deadline, deadline + microseconds(900), period);
    assert(deadline == start + 600 * period);
}
