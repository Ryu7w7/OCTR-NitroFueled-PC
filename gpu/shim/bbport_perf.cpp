// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_perf.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace BbPerf {
namespace {

constexpr double WindowSeconds = 5.0;

struct Window {
    unsigned frames = 0, long_frames = 0;
    double ms = 0.0;
    float worst = 0.0f;
    unsigned counts[Buckets] = {};
};

bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("BB_PERF_LOG");
        return !(v && v[0] == '0');
    }();
    return on;
}

} // namespace

void Frame(float ms) {
    if (!Enabled() || !(ms > 0.0f)) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Window w;
    static Clock::time_point start = Clock::now();
    if (ms >= Edges[Buckets - 1]) {
        ++w.long_frames;
    } else {
        int b = 0;
        while (ms >= Edges[b]) {
            ++b;
        }
        ++w.counts[b];
        ++w.frames;
        w.ms += ms;
        if (ms > w.worst) {
            w.worst = ms;
        }
    }
    const auto now = Clock::now();
    if (std::chrono::duration<double>(now - start).count() < WindowSeconds) {
        return;
    }
    start = now;
    if (w.frames || w.long_frames) {
        std::string h;
        for (int b = 0; b < Buckets; ++b) {
            if (b) {
                h += ',';
            }
            h += std::to_string(w.counts[b]);
        }
        std::printf("Perf: frames %u ms %.1f worst %.1f long %u h %s\n", w.frames, w.ms, w.worst,
                    w.long_frames, h.c_str());
        std::fflush(stdout);
    }
    w = Window{};
}

} // namespace BbPerf
