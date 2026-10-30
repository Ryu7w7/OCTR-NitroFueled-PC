// SPDX-License-Identifier: GPL-2.0-or-later
// bbport lab: BB_VK_OWN_SIGNALS (see bbport_native_signals.h).
#include "bbport_native_signals.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/types.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/kernel/process.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "bbport_native_memory.h"
#include "bbport_native_vk.h"
#include "bbport_thread_cpu.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace BbSignals {
namespace {
using namespace AmdGpu;

enum Kind : u8 { Eop, Eos, Release, Write, Wait, Flip, Dma, Retire, Zpass, KindCount };
const char* const kind_names[KindCount] = {"EOP labels", "EOS labels", "release labels",
                                           "writes", "memory waits", "flips", "DMA",
                                           "frames retired", "occlusion results"};

struct Op {
    Kind kind;
    u32 offset; // into Group::words: the packet (header first)
};

/// What one submission (or sceGnmSubmitDone) signals, run once the GPU has finished native
/// submission `k`.
struct Group {
    u64 k = 0;
    u64 frame = 0; // Retire
    std::vector<u32> words;
    std::vector<Op> ops;
};

std::mutex queue_mutex;
std::condition_variable queue_cv;
std::deque<std::unique_ptr<Group>> queue;

// stats, reported every 5 seconds
std::atomic<u64> ops_run[KindCount]{};
std::atomic<u64> groups_run{0}, packets_scanned{0}, bad_headers{0}, register_waits{0},
    gds_skipped{0}, dma_fills{0}, dma_copies{0}, dma_cp{0}, gpu_wait_ns{0}, mem_wait_ns{0},
    gpu_timeouts{0}, ib_followed{0}, unhandled[256]{};

// a label written straight into guest memory: one store (see liverpool.cpp StoreLabel)
void StoreLabel(void* address, u64 value, u32 num_bytes) {
    if (Core::Memory::Instance()->TryWriteBacking(address, &value, num_bytes)) {
        return;
    }
    if (num_bytes == 8 && (reinterpret_cast<uintptr_t>(address) & 7) == 0) {
        __atomic_store_n(static_cast<u64*>(address), value, __ATOMIC_RELEASE);
    } else if (num_bytes == 4 && (reinterpret_cast<uintptr_t>(address) & 3) == 0) {
        __atomic_store_n(static_cast<u32*>(address), u32(value), __ATOMIC_RELEASE);
    } else {
        auto* dst = static_cast<u8*>(address);
        for (u32 i = 0; i < num_bytes; ++i) {
            __atomic_store_n(dst + i, u8(value >> (8 * i)), __ATOMIC_RELEASE);
        }
    }
}

void Irq(Platform::InterruptId id) {
    Platform::IrqC::Instance()->Signal(id);
}

// compute dispatches in the graphics stream are not run: counted by program (OrbShdr hash)
std::mutex dispatch_mutex;
std::vector<std::pair<u64, u64>> dispatch_counts; // hash, count
u32 compute_pgm_lo = 0, compute_pgm_hi = 0; // game thread only (Scan)

u64 ProgramHash(u64 address) {
    if (!address) {
        return 0;
    }
    const auto* code = reinterpret_cast<const u32*>(address);
    if (code[0] != 0xBEEB03FF) {
        return 1; // no footer pointer
    }
    const u8* footer = reinterpret_cast<const u8*>(address) + (u64(code[1]) + 1) * 8;
    if (std::memcmp(footer, "OrbShdr", 7) != 0) {
        return 2;
    }
    u64 hash;
    std::memcpy(&hash, footer + 16, sizeof(hash));
    return hash;
}

void NoteDispatch() {
    const u64 hash = ProgramHash((u64(compute_pgm_lo) | u64(compute_pgm_hi) << 32) << 8);
    std::lock_guard lk(dispatch_mutex);
    for (auto& [h, n] : dispatch_counts) {
        if (h == hash) {
            ++n;
            return;
        }
    }
    dispatch_counts.push_back({hash, 1});
}

void Keep(Group& g, Kind kind, const u32* packet, u32 dwords) {
    g.ops.push_back({kind, u32(g.words.size())});
    g.words.insert(g.words.end(), packet, packet + dwords);
}

void Scan(const u32* p, std::size_t n, Group& g, int depth) {
    std::size_t i = 0;
    while (i < n) {
        const auto* header = reinterpret_cast<const PM4Header*>(p + i);
        if (header->type == 2) {
            ++i; // padding
            continue;
        }
        if (header->type != 3) {
            // a clobbered header (see liverpool.cpp): resume at the next plausible packet
            bad_headers.fetch_add(1, std::memory_order_relaxed);
            std::size_t skip = 1;
            while (i + skip < n && skip < 64) {
                const u32 word = p[i + skip];
                const u32 kind = word >> 30, opcode = (word >> 8) & 0xff;
                if (kind == 2 || (kind == 3 && opcode >= 0x10 && opcode <= 0x9f)) {
                    break;
                }
                ++skip;
            }
            i += skip;
            continue;
        }
        const u32 len = header->type3.NumWords() + 1;
        if (i + len > n) {
            bad_headers.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        packets_scanned.fetch_add(1, std::memory_order_relaxed);
        const PM4ItOpcode op = header->type3.opcode;
        switch (op) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            if (nop->header.count.Value() != 0 &&
                nop->data_block[0] == PM4CmdNop::PayloadType::PatchedFlip) {
                Keep(g, Flip, p + i, 1);
            }
            break;
        }
        case PM4ItOpcode::EventWriteEop:
            Keep(g, Eop, p + i, len);
            break;
        case PM4ItOpcode::EventWriteEos:
            Keep(g, Eos, p + i, len);
            break;
        case PM4ItOpcode::ReleaseMem:
            Keep(g, Release, p + i, len);
            break;
        case PM4ItOpcode::WriteData:
            Keep(g, Write, p + i, len);
            break;
        case PM4ItOpcode::WaitRegMem:
            Keep(g, Wait, p + i, len);
            break;
        case PM4ItOpcode::DmaData: {
            const auto* dma = reinterpret_cast<const PM4DmaData*>(header);
            if (dma->dst_addr_lo == 0x3022C) {
                dma_cp.fetch_add(1, std::memory_order_relaxed); // to a CP register: nothing
            } else {
                Keep(g, Dma, p + i, len);
            }
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* ib = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            ib_followed.fetch_add(1, std::memory_order_relaxed);
            if (depth < 4) {
                Scan(ib->Address<const u32>(), ib->ib_size, g, depth + 1);
            }
            break;
        }
        case PM4ItOpcode::SetShReg: {
            // COMPUTE_PGM_LO/HI (0x2E0C/0x2E0D: SH offsets 0x20C/0x20D), for NoteDispatch
            const u32 base = p[i + 1] & 0xffff;
            for (u32 r = 0; r + 2 < len; ++r) {
                if (base + r == 0x20C) {
                    compute_pgm_lo = p[i + 2 + r];
                } else if (base + r == 0x20D) {
                    compute_pgm_hi = p[i + 2 + r];
                }
            }
            break;
        }
        case PM4ItOpcode::DispatchDirect:
        case PM4ItOpcode::DispatchIndirect:
            NoteDispatch();
            unhandled[u32(op) & 0xff].fetch_add(1, std::memory_order_relaxed);
            break;
        // state and draws: the native renderer takes them from the engine
        case PM4ItOpcode::SetContextReg:
        case PM4ItOpcode::SetConfigReg:
        case PM4ItOpcode::SetUconfigReg:
        case PM4ItOpcode::ClearState:
        case PM4ItOpcode::ContextControl:
        case PM4ItOpcode::IndexType:
        case PM4ItOpcode::NumInstances:
        case PM4ItOpcode::DrawIndex2:
        case PM4ItOpcode::DrawIndexAuto:
        case PM4ItOpcode::DrawIndexOffset2:
        case PM4ItOpcode::IndexBase:
        case PM4ItOpcode::EventWrite: {
            // occlusion query results (the sun's glow and lens flares): as shadPS4 answers them
            const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            if (event->event_index.Value() == EventIndex::ZpassDone &&
                event->event_type.Value() == EventType::PixelPipeStatDump && len >= 4) {
                Keep(g, Zpass, p + i, len);
            }
            break; // else cache flushes and pipeline events
        }
        case PM4ItOpcode::IndexBufferSize:
        case PM4ItOpcode::AcquireMem:   // cache coherence
        case PM4ItOpcode::PfpSyncMe:
        case PM4ItOpcode::IncrementDeCounter:
        case PM4ItOpcode::WaitOnCeCounter:
            break;
        default:
            unhandled[u32(op) & 0xff].fetch_add(1, std::memory_order_relaxed);
            break;
        }
        i += len;
    }
}

bool TestWait(const PM4CmdWaitRegMem& w) {
    return w.TestValue(__atomic_load_n(w.Address<u32*>(), __ATOMIC_ACQUIRE));
}

void RunDma(const PM4DmaData& dma) {
    const bool to_memory =
        dma.dst_sel == DmaDataDst::Memory || dma.dst_sel == DmaDataDst::MemoryUsingL2;
    const bool from_memory =
        dma.src_sel == DmaDataSrc::Memory || dma.src_sel == DmaDataSrc::MemoryUsingL2;
    const u32 bytes = dma.NumBytes();
    if (!to_memory || dma.src_sel == DmaDataSrc::Gds) {
        gds_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    u8* dst = dma.DstAddress<u8*>();
    if (dma.src_sel == DmaDataSrc::Data) {
        dma_fills.fetch_add(1, std::memory_order_relaxed);
        if (bytes <= 8 && (bytes == 4 || bytes == 8)) {
            const u64 v = bytes == 8 ? (u64(dma.data) << 32 | dma.data) : dma.data;
            StoreLabel(dst, v, bytes);
            return;
        }
        for (u32 o = 0; o + 4 <= bytes; o += 4) {
            std::memcpy(dst + o, &dma.data, 4);
        }
    } else if (from_memory) {
        dma_copies.fetch_add(1, std::memory_order_relaxed);
        const u8* src = dma.SrcAddress<const u8*>();
        if (bytes == 4 || bytes == 8) {
            u64 v = 0;
            std::memcpy(&v, src, bytes);
            StoreLabel(dst, v, bytes); // counters and labels: one store
        } else {
            std::memmove(dst, src, bytes);
        }
    }
}

void RunOp(const Group& g, const Op& op) {
    const u32* packet = g.words.data() + op.offset;
    switch (op.kind) {
    case Eop: {
        const auto& eop = *reinterpret_cast<const PM4CmdEventWriteEop*>(packet);
        eop.SignalFence([](void* address, u64 data, u32 bytes) { StoreLabel(address, data, bytes); },
                        [] { Irq(Platform::InterruptId::GfxEop); });
        break;
    }
    case Eos: {
        const auto& eos = *reinterpret_cast<const PM4CmdEventWriteEos*>(packet);
        if (eos.command == PM4CmdEventWriteEos::Command::SignalFence) {
            eos.SignalFence([](void* address, u64 data, u32 bytes) { StoreLabel(address, data, bytes); });
        } else {
            gds_skipped.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
    case Release: {
        const auto& rel = *reinterpret_cast<const PM4CmdReleaseMem*>(packet);
        rel.SignalFence([] { Irq(Platform::InterruptId::GfxEop); },
                        [](VAddr, u32, u32) { gds_skipped.fetch_add(1, std::memory_order_relaxed); });
        break;
    }
    case Write: {
        const auto& w = *reinterpret_cast<const PM4CmdWriteData*>(packet);
        const u32 bytes = w.Size();
        if (bytes == 4 || bytes == 8) {
            u64 v = 0;
            std::memcpy(&v, w.data, bytes);
            StoreLabel(w.Address<void*>(), v, bytes);
        } else {
            std::memcpy(w.Address<void*>(), w.data, bytes);
        }
        break;
    }
    case Wait: {
        const auto& w = *reinterpret_cast<const PM4CmdWaitRegMem*>(packet);
        if (w.mem_space.Value() != PM4CmdWaitRegMem::MemSpace::Memory) {
            register_waits.fetch_add(1, std::memory_order_relaxed); // no registers: taken as met
            break;
        }
        if (TestWait(w)) {
            break;
        }
        const auto start = std::chrono::steady_clock::now();
        bool told = false;
        for (u32 spins = 0; !TestWait(w); ++spins) {
            if (spins < 64) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            if (!told && std::chrono::steady_clock::now() - start > std::chrono::seconds(3)) {
                told = true;
                std::printf("Native signals: a memory wait on %p (func %u ref %#x mask %#x) "
                            "is unmet after 3 s (value %#x)\n",
                            static_cast<void*>(w.Address<u32*>()), u32(w.function.Value()), w.ref,
                            w.mask, *w.Address<u32*>());
                std::fflush(stdout);
            }
        }
        mem_wait_ns.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count()),
                              std::memory_order_relaxed);
        break;
    }
    case Flip:
        if (OwnPresent()) {
            // the flip handler prepares the frame itself (Presenter::PrepareNativeFrame)
            Irq(Platform::InterruptId::GfxFlip);
        } else if (liverpool) {
            // it prepares the presenter's frame with shadPS4's scheduler: on its thread
            liverpool->SendCommand([] { Irq(Platform::InterruptId::GfxFlip); });
        }
        break;
    case Dma:
        RunDma(*reinterpret_cast<const PM4DmaData*>(packet));
        break;
    case Retire:
        // the frame's labels are written (stream order): the guest may reuse its memory
        Libraries::GnmDriver::NoteFramesRetired(g.frame);
        Libraries::GnmDriver::ReleaseSubmissionLock();
        break;
    case Zpass: {
        // No real query: every dump reads as a valid count that grew by a full screen since the
        // last one, so whatever the game measures counts as visible (liverpool.cpp does the same;
        // without it the results are never written and the sun has no glow or flares).
        static constexpr u64 ValidMask = 0x8000000000000000ULL, Step = 0x2FFFFFFULL;
        static u64 pixel_counter = 0;
        static const s32 pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
        u64* results = reinterpret_cast<const PM4CmdEventWrite*>(packet)->Address<u64*>();
        for (s32 n = 0; n < pairs; ++n, results += 2) {
            StoreLabel(results, pixel_counter | ValidMask, 8);
        }
        pixel_counter += Step;
        break;
    }
    case KindCount:
        break;
    }
    ops_run[op.kind].fetch_add(1, std::memory_order_relaxed);
}

void Report() {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(now - last).count();
    if (s < 5.0) {
        return;
    }
    last = now;
    char line[2048];
    int n = std::snprintf(line, sizeof(line), "Native signals: %.0f groups/s, %.0f packets/s scanned",
                          double(groups_run.exchange(0)) / s, double(packets_scanned.exchange(0)) / s);
    for (u32 k = 0; k < KindCount; ++k) {
        const u64 v = ops_run[k].exchange(0);
        if (v && n < int(sizeof(line)) - 64) {
            n += std::snprintf(line + n, sizeof(line) - n, ", %s %.0f/s", kind_names[k], double(v) / s);
        }
    }
    n += std::snprintf(line + n, sizeof(line) - n,
                       "; waited for the GPU %.0f ms/s, on memory %.0f ms/s; DMA fills %llu, copies "
                       "%llu, to CP %llu, GDS skipped %llu; register waits %llu; IBs %llu; bad "
                       "headers %llu; GPU wait timeouts %llu",
                       double(gpu_wait_ns.exchange(0)) / 1e6 / s, double(mem_wait_ns.exchange(0)) / 1e6 / s,
                       (unsigned long long)dma_fills.exchange(0), (unsigned long long)dma_copies.exchange(0),
                       (unsigned long long)dma_cp.exchange(0), (unsigned long long)gds_skipped.exchange(0),
                       (unsigned long long)register_waits.exchange(0),
                       (unsigned long long)ib_followed.exchange(0),
                       (unsigned long long)bad_headers.exchange(0),
                       (unsigned long long)gpu_timeouts.exchange(0));
    bool first = true;
    for (u32 o = 0; o < 256 && n < int(sizeof(line)) - 32; ++o) {
        if (const u64 v = unhandled[o].exchange(0)) {
            n += std::snprintf(line + n, sizeof(line) - n, "%s%#x x%llu", first ? "; other packets: " : ", ",
                               o, (unsigned long long)v);
            first = false;
        }
    }
    std::printf("%s\n", line);
    {
        std::lock_guard lk(dispatch_mutex);
        if (!dispatch_counts.empty()) {
            std::printf("Native signals: compute dispatches not run, by program:");
            for (const auto& [h, c] : dispatch_counts) {
                std::printf(" %016llx x%llu", (unsigned long long)h, (unsigned long long)c);
            }
            std::printf("\n");
            dispatch_counts.clear();
        }
    }
    std::fflush(stdout);
}

/// BB_VK_EARLY_LABELS=1: labels, writes, DMA and memory waits run as soon as the submission is
/// captured (the native renderer copied what it reads from guest memory by then: constants,
/// vertices, indices, texture snapshots), not once the GPU has run it. The game (Bink in the track
/// select) waits on those labels before it reuses its buffers. Flips and frame ends still wait
/// for the GPU (on a thread of their own), which keeps the game's pace and the presented frames.
bool EarlyLabels() {
    static const bool on = [] {
        const char* env = std::getenv("BB_VK_EARLY_LABELS");
        return env && env[0] == '1';
    }();
    return on;
}

struct GatedOp {
    u64 k;
    Kind kind;
    u64 frame;
};
std::mutex gated_mutex;
std::condition_variable gated_cv;
std::deque<GatedOp> gated;

void GatedWorker() {
    BbCpu::Register("bb:NativeSignalsGpu");
    for (;;) {
        GatedOp op;
        {
            std::unique_lock lk(gated_mutex);
            gated_cv.wait(lk, [] { return !gated.empty(); });
            op = gated.front();
            gated.pop_front();
        }
        const auto start = std::chrono::steady_clock::now();
        while (!BbNativeVk::WaitSubmissionDone(op.k, 5'000'000'000ull)) {
            gpu_timeouts.fetch_add(1, std::memory_order_relaxed);
            std::printf("Native signals: native submission %llu not done after 5 s\n",
                        (unsigned long long)op.k);
            std::fflush(stdout);
        }
        gpu_wait_ns.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count()),
                              std::memory_order_relaxed);
        Group g;
        g.k = op.k;
        g.frame = op.frame;
        RunOp(g, Op{op.kind, 0});
    }
}

void Worker() {
    BbCpu::Register("bb:NativeSignals");
    if (EarlyLabels()) {
        std::printf("Native signals: BB_VK_EARLY_LABELS=1: labels written once a submission is "
                    "captured; flips and frame ends after the GPU\n");
        std::thread(GatedWorker).detach();
    }
    for (;;) {
        std::unique_ptr<Group> g;
        {
            std::unique_lock lk(queue_mutex);
            if (!queue_cv.wait_for(lk, std::chrono::seconds(1), [] { return !queue.empty(); })) {
                lk.unlock();
                Report();
                continue;
            }
            g = std::move(queue.front());
            queue.pop_front();
        }
        if (EarlyLabels()) {
            for (const Op& op : g->ops) {
                if (op.kind == Flip || op.kind == Retire) {
                    {
                        std::lock_guard lk(gated_mutex);
                        gated.push_back(GatedOp{g->k, op.kind, g->frame});
                    }
                    gated_cv.notify_one();
                } else {
                    RunOp(*g, op);
                }
            }
            groups_run.fetch_add(1, std::memory_order_relaxed);
            Report();
            continue;
        }
        const auto start = std::chrono::steady_clock::now();
        // as an end-of-pipe event: the work submitted before it is done
        while (!BbNativeVk::WaitSubmissionDone(g->k, 5'000'000'000ull)) {
            gpu_timeouts.fetch_add(1, std::memory_order_relaxed);
            std::printf("Native signals: native submission %llu not done after 5 s\n",
                        (unsigned long long)g->k);
            std::fflush(stdout);
        }
        gpu_wait_ns.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count()),
                              std::memory_order_relaxed);
        for (const Op& op : g->ops) {
            RunOp(*g, op);
        }
        groups_run.fetch_add(1, std::memory_order_relaxed);
        Report();
    }
}

void Push(std::unique_ptr<Group> g) {
    {
        std::lock_guard lk(queue_mutex);
        queue.push_back(std::move(g));
    }
    queue_cv.notify_one();
}
} // namespace

std::atomic<bool> on_flag{false};

bool On() {
    return on_flag.load(std::memory_order_acquire);
}

bool OwnPresent() {
    static const bool own = [] {
        const char* env = std::getenv("BB_VK_OWN_PRESENT");
        return (env && env[0] == '1') || BbMem::NativeOnly();
    }();
    return own && On();
}

bool Decide() {
    static const bool on = [] {
        const char* env = std::getenv("BB_VK_OWN_SIGNALS");
        if (!(env && env[0] == '1') && !BbMem::NativeOnly()) {
            return false;
        }
        const char* why = nullptr;
        if (!BbNativeVk::DrawsAlone() || !BbNativeVk::Drawing()) {
            why = "the native renderer does not draw alone (BB_VK_ALONE=1, BB_VK_SHOW=1)";
        } else if (!BbNativeVk::GpuTimeline()) {
            why = "no timeline semaphore";
        }
        if (why) {
            std::printf("Native signals: BB_VK_OWN_SIGNALS=1 ignored: %s\n", why);
            if (BbMem::NativeOnly()) {
                // no Rasterizer: shadPS4's command processor cannot run the frames either
                std::printf("Native signals: BB_VK_NATIVE_ONLY=1 cannot run without them; stopping\n");
                std::fflush(stdout);
                std::abort();
            }
            return false;
        }
        std::printf("Native signals: BB_VK_OWN_SIGNALS=1: labels, waits, DMA and flips run by the "
                    "native renderer; shadPS4's command processor gets no graphics submissions\n");
        std::thread(Worker).detach();
        on_flag.store(true, std::memory_order_release);
        return true;
    }();
    return on;
}

void Submit(std::span<const std::uint32_t> dcb) {
    auto g = std::make_unique<Group>();
    g->k = BbNativeVk::SubmissionsQueued();
    Scan(dcb.data(), dcb.size(), *g, 0);
    if (!g->ops.empty()) {
        Push(std::move(g));
    }
}

void FrameDone(std::uint64_t frame) {
    auto g = std::make_unique<Group>();
    g->k = BbNativeVk::SubmissionsQueued();
    g->frame = frame;
    g->ops.push_back({Retire, 0});
    Push(std::move(g));
}
} // namespace BbSignals
