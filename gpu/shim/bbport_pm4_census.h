// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Lab: what shadPS4's command processor still does while the native renderer draws alone (the
// work to replace before the native renderer can run without it). Packets are counted per queue
// (graphics, compute, constant engine) and opcode; the native report prints them per second.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace BbPm4 {

enum Queue { Graphics = 0, Compute = 1, ConstantEngine = 2, Queues = 3 };

inline std::array<std::array<std::atomic<std::uint64_t>, 256>, Queues> packets{};
/// time from each packet's start to the next one's (waits inside a packet included); opcode 0 is
/// the command processor's own work between packets (ProcessCommands)
inline std::array<std::array<std::atomic<std::uint64_t>, 256>, Queues> nanos{};

struct Clock {
    int queue = -1;
    unsigned opcode = 0;
    std::chrono::steady_clock::time_point since{};
};
inline thread_local Clock lap_clock;

/// The time since the last lap goes to the packet that was running; `opcode` < 0 stops the clock
/// (the queue ran out of packets: idle time is nobody's).
inline void Lap(Queue q, int opcode) {
    const auto now = std::chrono::steady_clock::now();
    if (lap_clock.queue >= 0) {
        nanos[lap_clock.queue][lap_clock.opcode].fetch_add(
            std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - lap_clock.since).count()),
            std::memory_order_relaxed);
    }
    lap_clock.queue = opcode < 0 ? -1 : int(q);
    lap_clock.opcode = opcode < 0 ? 0u : unsigned(opcode) & 0xffu;
    lap_clock.since = now;
}

/// Compute dispatches shadPS4 runs, by program hash (Rasterizer::DispatchRecord, HLE ones too).
inline std::mutex dispatch_mutex;
inline std::vector<std::pair<std::uint64_t, std::uint64_t>> dispatch_counts; // hash, count

inline void CountDispatch(std::uint64_t hash) {
    std::lock_guard lk(dispatch_mutex);
    for (auto& [h, n] : dispatch_counts) {
        if (h == hash) {
            ++n;
            return;
        }
    }
    dispatch_counts.emplace_back(hash, 1);
}

/// "0xfefebf9f 410/s, 0xded6d46b 90/s, ..." since the last call.
inline std::string DispatchReport(double seconds) {
    static std::vector<std::pair<std::uint64_t, std::uint64_t>> last;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> now;
    {
        std::lock_guard lk(dispatch_mutex);
        now = dispatch_counts;
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> delta;
    for (const auto& [h, n] : now) {
        std::uint64_t before = 0;
        for (const auto& [lh, ln] : last) {
            if (lh == h) {
                before = ln;
            }
        }
        if (n > before) {
            delta.emplace_back(n - before, h);
        }
    }
    last = now;
    std::sort(delta.rbegin(), delta.rend());
    std::string out;
    char buf[64];
    for (const auto& [n, h] : delta) {
        std::snprintf(buf, sizeof(buf), "%s%#llx %.0f/s", out.empty() ? "" : ", ",
                      static_cast<unsigned long long>(h), n / (seconds > 0 ? seconds : 1.0));
        out += buf;
    }
    return out.empty() ? "none" : out;
}

/// Phases of the command processor's main loop (Liverpool::Process), wall time each.
enum Phase { LabelsWait, IdleWait, FrameStart, Commands, Decode, AfterDecode, SubmitEnd, EndFlush,
             EndFrames, EndDrain, EndDeferred, EndIdle, OsDrain, OsFaults, OsDownloads, OsGarbage,
             OsTick, OsFlush, Phases };
inline std::array<std::atomic<std::uint64_t>, Phases> phase_ns{};
inline thread_local int phase_now = -1;
inline thread_local std::chrono::steady_clock::time_point phase_since{};

inline void PhaseLap(Phase p) {
    const auto now = std::chrono::steady_clock::now();
    if (phase_now >= 0) {
        phase_ns[phase_now].fetch_add(
            std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - phase_since).count()),
            std::memory_order_relaxed);
    }
    phase_now = int(p);
    phase_since = now;
}

/// "labels wait 3%, idle wait 10%, ..." since the last call, in % of wall time.
inline std::string PhaseReport(double seconds) {
    static const char* const names[Phases] = {"labels wait", "idle wait", "frame start", "commands",
                                              "decode", "after decode", "submission end",
                                              "end: submit+flush", "end: frame signals",
                                              "end: draw pipe", "end: deferred writes",
                                              "end: idle signal", "on submit: draw pipe",
                                              "on submit: faults", "on submit: downloads",
                                              "on submit: garbage collector", "on submit: tick",
                                              "flush"};
    static std::array<std::uint64_t, Phases> last{};
    std::string out;
    char buf[64];
    for (int p = 0; p < Phases; ++p) {
        const std::uint64_t now = phase_ns[p].load(std::memory_order_relaxed);
        const double share = (now - last[p]) / ((seconds > 0 ? seconds : 1.0) * 1e9) * 100.0;
        last[p] = now;
        if (share < 0.05) {
            continue;
        }
        std::snprintf(buf, sizeof(buf), "%s%s %.1f%%", out.empty() ? "" : ", ", names[p], share);
        out += buf;
    }
    return out;
}

inline void Count(Queue q, unsigned opcode) {
    packets[q][opcode & 0xffu].fetch_add(1, std::memory_order_relaxed);
    Lap(q, int(opcode & 0xffu));
}

inline const char* Name(unsigned op) {
    switch (op) {
    case 0x00: return "(between packets)";
    case 0x10: return "Nop";
    case 0x11: return "SetBase";
    case 0x12: return "ClearState";
    case 0x13: return "IndexBufferSize";
    case 0x15: return "DispatchDirect";
    case 0x16: return "DispatchIndirect";
    case 0x1D: return "AtomicGds";
    case 0x1E: return "Atomic";
    case 0x1F: return "OcclusionQuery";
    case 0x20: return "SetPredication";
    case 0x21: return "RegRmw";
    case 0x22: return "CondExec";
    case 0x23: return "PredExec";
    case 0x24: return "DrawIndirect";
    case 0x25: return "DrawIndexIndirect";
    case 0x26: return "IndexBase";
    case 0x27: return "DrawIndex2";
    case 0x28: return "ContextControl";
    case 0x2A: return "IndexType";
    case 0x2C: return "DrawIndirectMulti";
    case 0x2D: return "DrawIndexAuto";
    case 0x2F: return "NumInstances";
    case 0x30: return "DrawIndexMultiAuto";
    case 0x33: return "IndirectBufferConst";
    case 0x34: return "StrmoutBufferUpdate";
    case 0x35: return "DrawIndexOffset2";
    case 0x37: return "WriteData";
    case 0x38: return "DrawIndexIndirectMulti";
    case 0x39: return "MemSemaphore";
    case 0x3C: return "WaitRegMem";
    case 0x3F: return "IndirectBuffer";
    case 0x40: return "CopyData";
    case 0x41: return "CommandProcessorDma";
    case 0x42: return "PfpSyncMe";
    case 0x43: return "SurfaceSync";
    case 0x45: return "CondWrite";
    case 0x46: return "EventWrite";
    case 0x47: return "EventWriteEop";
    case 0x48: return "EventWriteEos";
    case 0x49: return "ReleaseMem";
    case 0x4A: return "PreambleCntl";
    case 0x50: return "DmaData";
    case 0x51: return "ContextRegRmw";
    case 0x58: return "AcquireMem";
    case 0x59: return "Rewind";
    case 0x5F: return "LoadShReg";
    case 0x60: return "LoadConfigReg";
    case 0x61: return "LoadContextReg";
    case 0x68: return "SetConfigReg";
    case 0x69: return "SetContextReg";
    case 0x73: return "SetContextRegIndirect";
    case 0x76: return "SetShReg";
    case 0x77: return "SetShRegOffset";
    case 0x78: return "SetQueueReg";
    case 0x79: return "SetUconfigReg";
    case 0x80: return "LoadConstRam";
    case 0x81: return "WriteConstRam";
    case 0x83: return "DumpConstRam";
    case 0x84: return "IncrementCeCounter";
    case 0x85: return "IncrementDeCounter";
    case 0x86: return "WaitOnCeCounter";
    case 0x88: return "WaitOnDeCounterDiff";
    case 0x8E: return "GetLodStats";
    case 0x9D: return "DrawIndexIndirectCountMulti";
    default: return nullptr;
    }
}

/// "graphics: SetShReg 1234/s, ... | compute: ..." for the packets since the last call, per second.
inline std::string Report(double seconds) {
    static std::array<std::array<std::uint64_t, 256>, Queues> last{};
    static const char* const queue_names[Queues] = {"graphics", "compute", "constant engine"};
    std::string out;
    char buf[96];
    for (int q = 0; q < Queues; ++q) {
        std::string line;
        for (unsigned op = 0; op < 256; ++op) {
            const std::uint64_t now = packets[q][op].load(std::memory_order_relaxed);
            const std::uint64_t n = now - last[q][op];
            last[q][op] = now;
            if (!n) {
                continue;
            }
            const char* name = Name(op);
            if (name) {
                std::snprintf(buf, sizeof(buf), "%s%s %.0f/s", line.empty() ? "" : ", ", name,
                              double(n) / (seconds > 0 ? seconds : 1.0));
            } else {
                std::snprintf(buf, sizeof(buf), "%s0x%02x %.0f/s", line.empty() ? "" : ", ", op,
                              double(n) / (seconds > 0 ? seconds : 1.0));
            }
            line += buf;
        }
        if (!line.empty()) {
            out += out.empty() ? "" : " | ";
            out += std::string(queue_names[q]) + ": " + line;
        }
    }
    return out.empty() ? "none" : out;
}

/// "DmaData 31.2%, SetContextReg 12.0%, ..." : the share of wall time each packet type took on the
/// command processor since the last call (the ten largest, all queues together).
inline std::string TimeReport(double seconds) {
    static std::array<std::array<std::uint64_t, 256>, Queues> last{};
    std::vector<std::pair<std::uint64_t, unsigned>> order;
    std::array<std::uint64_t, 256> sum{};
    std::uint64_t total = 0;
    for (int q = 0; q < Queues; ++q) {
        for (unsigned op = 0; op < 256; ++op) {
            const std::uint64_t now = nanos[q][op].load(std::memory_order_relaxed);
            sum[op] += now - last[q][op];
            last[q][op] = now;
        }
    }
    for (unsigned op = 0; op < 256; ++op) {
        if (sum[op]) {
            order.emplace_back(sum[op], op);
            total += sum[op];
        }
    }
    std::sort(order.rbegin(), order.rend());
    const double wall = (seconds > 0 ? seconds : 1.0) * 1e9;
    std::string out;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "busy %.0f%% of the time: ", total / wall * 100.0);
    out += buf;
    for (std::size_t k = 0; k < order.size() && k < 10; ++k) {
        const char* name = Name(order[k].second);
        if (name) {
            std::snprintf(buf, sizeof(buf), "%s%s %.1f%%", k ? ", " : "", name, order[k].first / wall * 100.0);
        } else {
            std::snprintf(buf, sizeof(buf), "%s0x%02x %.1f%%", k ? ", " : "", order[k].second,
                          order[k].first / wall * 100.0);
        }
        out += buf;
    }
    return out;
}

} // namespace BbPm4
