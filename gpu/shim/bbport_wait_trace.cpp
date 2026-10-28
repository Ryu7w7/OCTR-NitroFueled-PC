// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_wait_trace.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#endif

namespace BbWaitTrace {
namespace {
constexpr int Skip = 1; // Scope::Scope itself
constexpr int Kept = 6; // frames that identify a stack

struct Key {
    std::array<void*, Kept> frames{};
    std::array<char, 16> thread{};
    bool operator<(const Key& other) const {
        if (frames != other.frames) {
            return frames < other.frames;
        }
        return thread < other.thread;
    }
};
struct Total {
    std::uint64_t ns = 0, count = 0;
};
std::mutex mutex;
std::map<Key, Total> totals;

#ifdef _WIN32
int CaptureStack(void** frames, int count) {
    return RtlCaptureStackBackTrace(0, DWORD(count), frames, nullptr);
}

/// "sacredfire.exe+0x1ca380": module and offset, for llvm-symbolizer (--adjust-vma with the
/// image base) on the build's executable.
std::string Describe(void* address) {
    HMODULE module = nullptr;
    char text[300];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(address), &module)) {
        std::snprintf(text, sizeof(text), "%p", address);
        return text;
    }
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* file = std::strrchr(path, '\\');
    file = file ? file + 1 : path;
    std::snprintf(text, sizeof(text), "%s+%#llx", file,
                  (unsigned long long)(static_cast<char*>(address) - reinterpret_cast<char*>(module)));
    return text;
}

void ThreadName(std::array<char, 16>& out) {
    std::snprintf(out.data(), out.size(), "tid %lu", (unsigned long)GetCurrentThreadId());
}
#else
int CaptureStack(void** frames, int count) {
    return backtrace(frames, count);
}

std::string Describe(void* address) {
    Dl_info info{};
    if (!dladdr(address, &info)) {
        char text[32];
        std::snprintf(text, sizeof(text), "%p", address);
        return text;
    }
    const char* file = info.dli_fname ? std::strrchr(info.dli_fname, '/') : nullptr;
    file = file ? file + 1 : (info.dli_fname ? info.dli_fname : "?");
    char text[512];
    if (info.dli_sname) {
        std::snprintf(text, sizeof(text), "%s+%#lx", info.dli_sname,
                      (unsigned long)(static_cast<char*>(address) -
                                      static_cast<char*>(info.dli_saddr)));
    } else {
        std::snprintf(text, sizeof(text), "%s+%#lx", file,
                      (unsigned long)(static_cast<char*>(address) -
                                      static_cast<char*>(info.dli_fbase)));
    }
    return text;
}

void ThreadName(std::array<char, 16>& out) {
    pthread_getname_np(pthread_self(), out.data(), out.size());
}
#endif
} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_WAIT_TRACE");
        return env && env[0] == '1';
    }();
    return enabled;
}

Scope::Scope() {
    if (!Enabled()) {
        return;
    }
    depth = CaptureStack(frames, int(std::size(frames)));
    start = std::chrono::steady_clock::now();
}

Scope::~Scope() {
    if (depth == 0) {
        return;
    }
    const auto ns = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count());
    Key key;
    for (int i = 0; i < Kept && Skip + i < depth; ++i) {
        key.frames[i] = frames[Skip + i];
    }
    ThreadName(key.thread);
    std::scoped_lock lk{mutex};
    auto& total = totals[key];
    total.ns += ns;
    ++total.count;
}

void Report(double window_s) {
    if (!Enabled()) {
        return;
    }
    std::vector<std::pair<Key, Total>> list;
    {
        std::scoped_lock lk{mutex};
        list.assign(totals.begin(), totals.end());
        totals.clear();
    }
    std::sort(list.begin(), list.end(),
              [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    std::uint64_t all = 0;
    for (const auto& [key, total] : list) {
        all += total.ns;
    }
    std::printf("GPU tick waits: %.1f%% of the time in all, by stack:\n",
                window_s > 0 ? all / (window_s * 1e7) : 0.0);
    for (std::size_t i = 0; i < list.size() && i < 6; ++i) {
        const auto& [key, total] = list[i];
        std::printf("  %5.1f%% %6llu waits, thread %s:", total.ns / (window_s * 1e7),
                    (unsigned long long)total.count, key.thread.data());
        for (void* frame : key.frames) {
            if (frame) {
                std::printf(" <- %s", Describe(frame).c_str());
            }
        }
        std::printf("\n");
    }
}
} // namespace BbWaitTrace
