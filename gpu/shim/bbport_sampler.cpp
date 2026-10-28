// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_sampler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace BbSampler {
#ifdef _WIN32
namespace {
struct Watched {
    std::string name;
    HANDLE handle = nullptr;
    std::unordered_map<std::uint64_t, std::uint64_t> self, total; // exe offset -> samples
    // samples in system code: by the exe's first caller of it, and by module
    std::unordered_map<std::uint64_t, std::uint64_t> via;
    std::unordered_map<std::string, std::uint64_t> modules;
    std::uint64_t samples = 0, outside = 0;
};

std::mutex mutex;
std::vector<Watched*> watched;
std::uintptr_t image_base = 0, image_end = 0;

const std::vector<std::string>& Wanted() {
    static const std::vector<std::string> parts = [] {
        std::vector<std::string> out;
        const char* env = std::getenv("BB_VK_PROFILE");
        if (!env) {
            return out;
        }
        std::string s = env;
        for (std::size_t at = 0; at <= s.size();) {
            const std::size_t end = std::min(s.find(',', at), s.size());
            if (end > at) {
                out.push_back(s.substr(at, end - at));
            }
            at = end + 1;
        }
        return out;
    }();
    return parts;
}

bool InImage(DWORD64 pc) {
    return pc >= image_base && pc < image_end;
}

std::string ModuleName(DWORD64 pc) {
    static std::unordered_map<std::uintptr_t, std::string> names;
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(pc), &m)) {
        return "?";
    }
    auto& name = names[reinterpret_cast<std::uintptr_t>(m)];
    if (name.empty()) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(m, path, MAX_PATH);
        name = path;
        name = name.substr(name.find_last_of("\\/") + 1);
    }
    return name;
}

constexpr int MaxFrames = 16;

/// The suspended thread's pc and up to 15 return addresses (each module's unwind data).
int Walk(CONTEXT ctx, DWORD64* pcs) {
    int n = 0;
    for (; n < MaxFrames; ++n) {
        const DWORD64 pc = ctx.Rip;
        if (!pc) {
            break;
        }
        pcs[n] = pc;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &base, nullptr);
        if (!fn) {
            if (!InImage(pc) && n > 0) {
                ++n;
                break; // unknown code without unwind data
            }
            // a leaf function: the return address is on top of the stack
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        PVOID handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, fn, &ctx, &handler_data, &frame, nullptr);
    }
    return n;
}

void Print(Watched& w, double seconds) {
    auto top = [](const std::unordered_map<std::uint64_t, std::uint64_t>& m, std::size_t k) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (v.size() > k) {
            v.resize(k);
        }
        return v;
    };
    std::string line = "Profile " + w.name + ": " + std::to_string(w.samples) + " samples in " +
                       std::to_string(int(seconds)) + " s (" + std::to_string(w.outside) +
                       " outside the exe); self:";
    for (const auto& [off, n] : top(w.self, 40)) {
        char b[64];
        std::snprintf(b, sizeof(b), " %#llx:%.1f%%", (unsigned long long)off, 100.0 * n / w.samples);
        line += b;
    }
    line += "; total:";
    for (const auto& [off, n] : top(w.total, 60)) {
        char b[64];
        std::snprintf(b, sizeof(b), " %#llx:%.1f%%", (unsigned long long)off, 100.0 * n / w.samples);
        line += b;
    }
    line += "; system code by caller:";
    for (const auto& [off, n] : top(w.via, 30)) {
        char b[64];
        std::snprintf(b, sizeof(b), " %#llx:%.1f%%", (unsigned long long)off, 100.0 * n / w.samples);
        line += b;
    }
    line += "; modules:";
    for (const auto& [m, n] : w.modules) {
        char b[160];
        std::snprintf(b, sizeof(b), " %s %.1f%%", m.c_str(), 100.0 * n / w.samples);
        line += b;
    }
    std::printf("%s\n", line.c_str());
    w.self.clear();
    w.total.clear();
    w.via.clear();
    w.modules.clear();
    w.samples = w.outside = 0;
}

void Run() {
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::lock_guard lk(mutex);
        for (Watched* w : watched) {
            if (SuspendThread(w->handle) == DWORD(-1)) {
                continue;
            }
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            DWORD64 pcs[MaxFrames];
            int n = 0;
            if (GetThreadContext(w->handle, &ctx)) {
                n = Walk(ctx, pcs);
            }
            ResumeThread(w->handle);
            if (!n) {
                continue;
            }
            ++w->samples;
            if (!InImage(pcs[0])) {
                ++w->outside;
                ++w->modules[ModuleName(pcs[0])];
                for (int i = 1; i < n; ++i) {
                    if (InImage(pcs[i])) {
                        ++w->via[pcs[i] - image_base - 1];
                        break;
                    }
                }
            } else {
                ++w->self[pcs[0] - image_base];
            }
            std::uint64_t seen[MaxFrames];
            int k = 0;
            for (int i = 0; i < n; ++i) {
                if (!InImage(pcs[i])) {
                    continue;
                }
                // return addresses point after the call: one byte back names the call's line
                const std::uint64_t off = pcs[i] - image_base - (i ? 1 : 0);
                if (std::find(seen, seen + k, off) == seen + k) {
                    seen[k++] = off;
                    ++w->total[off];
                }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(10)) {
            const double s = std::chrono::duration<double>(now - last).count();
            last = now;
            for (Watched* w : watched) {
                if (w->samples) {
                    Print(*w, s);
                }
            }
            std::fflush(stdout);
        }
    }
}
} // namespace

void Register(const char* name) {
    if (!name || Wanted().empty()) {
        return;
    }
    const std::string n = name;
    if (std::none_of(Wanted().begin(), Wanted().end(),
                     [&](const std::string& p) { return n.find(p) != std::string::npos; })) {
        return;
    }
    HANDLE h = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                         0)) {
        return;
    }
    static std::once_flag once;
    std::call_once(once, [] {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const std::uint8_t*>(dos) + dos->e_lfanew);
        image_base = reinterpret_cast<std::uintptr_t>(dos);
        image_end = image_base + nt->OptionalHeader.SizeOfImage;
        std::printf("Profile: sampling threads named like BB_VK_PROFILE (exe base %#llx)\n",
                    (unsigned long long)image_base);
        std::thread(Run).detach();
    });
    std::lock_guard lk(mutex);
    watched.push_back(new Watched{.name = n, .handle = h});
}
#else
void Register(const char*) {}
#endif
} // namespace BbSampler
