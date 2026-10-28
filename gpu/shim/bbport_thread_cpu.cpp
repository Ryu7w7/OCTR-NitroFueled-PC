// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_thread_cpu.h"
#include "bbport_sampler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace BbCpu {
namespace {

struct Thread {
    std::string name;
#ifdef _WIN32
    HANDLE handle = nullptr;
#else
    clockid_t clock{};
#endif
    std::uint64_t last_ns = 0;
};

std::mutex mutex;
std::vector<Thread> threads;
std::uint64_t last_process_ns = 0;
std::chrono::steady_clock::time_point last_wall = std::chrono::steady_clock::now();

#ifdef _WIN32
std::uint64_t FileTimeNs(const FILETIME& t) {
    return ((std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 100;
}

std::uint64_t ThreadNs(const Thread& t) {
    FILETIME c, e, k, u;
    return GetThreadTimes(t.handle, &c, &e, &k, &u) ? FileTimeNs(k) + FileTimeNs(u) : 0;
}

std::uint64_t ProcessNs() {
    FILETIME c, e, k, u;
    return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? FileTimeNs(k) + FileTimeNs(u) : 0;
}
#else
std::uint64_t ClockNs(clockid_t clock) {
    timespec ts{};
    return clock_gettime(clock, &ts) == 0 ? std::uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec : 0;
}

std::uint64_t ThreadNs(const Thread& t) {
    return ClockNs(t.clock);
}

std::uint64_t ProcessNs() {
    return ClockNs(CLOCK_PROCESS_CPUTIME_ID);
}
#endif

} // namespace

void Register(const char* name) {
    BbSampler::Register(name); // lab: BB_VK_PROFILE
    Thread t;
    t.name = name ? name : "?";
#ifdef _WIN32
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t.handle,
                         THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        return;
    }
#else
    if (pthread_getcpuclockid(pthread_self(), &t.clock) != 0) {
        return;
    }
#endif
    t.last_ns = ThreadNs(t);
    std::lock_guard lk(mutex);
    if (!last_process_ns) { // the first report covers from here, not from the process start
        last_process_ns = ProcessNs();
        last_wall = std::chrono::steady_clock::now();
    }
    threads.push_back(t);
}

std::string Report() {
    std::lock_guard lk(mutex);
    const auto now = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(now - last_wall).count();
    last_wall = now;
    if (wall <= 0.0) {
        return "";
    }
    std::map<std::string, std::uint64_t> by_name;
    std::uint64_t named = 0;
    for (Thread& t : threads) {
        const std::uint64_t ns = ThreadNs(t);
        const std::uint64_t d = ns >= t.last_ns ? ns - t.last_ns : 0;
        t.last_ns = ns;
        by_name[t.name] += d;
        named += d;
    }
    const std::uint64_t process = ProcessNs();
    const std::uint64_t pd = process >= last_process_ns ? process - last_process_ns : 0;
    last_process_ns = process;
    std::vector<std::pair<std::uint64_t, std::string>> order;
    for (const auto& [name, d] : by_name) {
        if (d * 100.0 / 1e9 / wall >= 1.0) { // threads under 1% of a core left out
            order.emplace_back(d, name);
        }
    }
    std::sort(order.rbegin(), order.rend());
    std::string out;
    char buf[160];
    for (const auto& [d, name] : order) {
        std::snprintf(buf, sizeof(buf), "%s%s %.0f%%", out.empty() ? "" : ", ", name.c_str(),
                      d / 1e9 / wall * 100.0);
        out += buf;
    }
    std::snprintf(buf, sizeof(buf), "%sgame+rest %.0f%% (process %.0f%%)", out.empty() ? "" : ", ",
                  (pd > named ? pd - named : 0) / 1e9 / wall * 100.0, pd / 1e9 / wall * 100.0);
    out += buf;
    return out;
}

} // namespace BbCpu
