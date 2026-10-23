// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_clobber.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <pthread.h>
#ifndef _WIN32
#include <sys/mman.h>
#ifdef __APPLE__
#include <sys/ucontext.h> // <ucontext.h> needs _XOPEN_SOURCE on macOS
#else
#include <ucontext.h>
#endif
#endif
#include <unistd.h>

namespace BbClobber {
namespace {
constexpr std::size_t Page = 4096;

std::uint64_t NowNs() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}

const char* SourceName(std::uint32_t source) {
    switch (source) {
    case Eop: return "EOP fence";
    case Eos: return "EOS fence";
    case EosGds: return "EOS GDS store";
    case ReleaseMem: return "RELEASE_MEM fence";
    case WriteData: return "WRITE_DATA";
    case ComputeWriteData: return "compute WRITE_DATA";
    case DmaFill: return "DMA fill";
    case DmaCopy: return "DMA copy";
    case BufferDownload: return "buffer download";
    case ImageDownload: return "image download";
    case ConstRam: return "DUMP_CONST_RAM";
    default: return "?";
    }
}

// Submitted command buffers (SubmitGfx order); the game reuses their memory every frame.
struct Range {
    std::uint64_t seq, base, bytes, ns;
};
std::array<Range, 256> ranges;
std::atomic<std::uint64_t> ranges_head{0};
std::atomic<std::uint64_t> decoding{0};

// The port's writes into guest memory.
struct Write {
    std::uint64_t address, value, ns, decoding;
    std::uint32_t bytes, source, tid, pad;
};
std::array<Write, 16384> writes;
std::atomic<std::uint64_t> writes_head{0};
std::atomic<std::uint64_t> hits{0};

// BB_COPY_PROTECT: the read-only copies.
struct Protected {
    std::atomic<std::uint64_t> base{0}, bytes{0}, seq{0};
};
std::array<Protected, 64> protected_copies;

std::size_t RoundUp(std::size_t bytes) {
    return (bytes + Page - 1) & ~(Page - 1);
}
} // namespace

std::int64_t FirstBadHeader(const std::uint32_t* words, std::uint64_t dwords) {
    std::uint64_t at = 0;
    while (at < dwords) {
        const std::uint32_t header = words[at];
        switch (header >> 30) {
        case 3:
            at += ((header >> 16) & 0x3fff) + 2;
            break;
        case 2:
            ++at;
            break;
        default:
            return std::int64_t(at);
        }
    }
    return -1;
}

void NoteSubmit(std::uint64_t seq, const void* guest_dcb, std::uint64_t bytes) {
    ranges[ranges_head.fetch_add(1, std::memory_order_relaxed) % ranges.size()] =
        Range{seq, reinterpret_cast<std::uint64_t>(guest_dcb), bytes, NowNs()};
    static const bool test = [] {
        const char* env = std::getenv("BB_ISSUE2_TEST");
        return env && env[0] == '1';
    }();
    if (test) {
        AskForReport();
    }
}

void NoteDecoding(std::uint64_t seq) {
    decoding.store(seq, std::memory_order_relaxed);
}

namespace {
// The dcb being decoded, for StaleLabel, and the ranges of it the port wrote since (NoteWrite).
struct DecodingCopy {
    std::uint64_t seq = 0, base = 0, dwords = 0;
    std::shared_ptr<const std::uint32_t> words;
    std::array<std::pair<std::uint64_t, std::uint64_t>, 512> touched{};
    std::size_t touched_count = 0;
    bool touched_overflow = false;
};
std::mutex decoding_copy_mutex;
DecodingCopy decoding_copy;
std::atomic<std::uint64_t> decoding_base{0}, decoding_end{0};
std::atomic<std::uint64_t> stale_skipped{0};

// NoteWrite: a port write inside the dcb being decoded. Holds no lock.
void NoteTouched(std::uint64_t address, std::uint64_t bytes) {
    if (address + bytes <= decoding_base.load(std::memory_order_relaxed) ||
        address >= decoding_end.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock{decoding_copy_mutex};
    DecodingCopy& d = decoding_copy;
    if (d.touched_count < d.touched.size()) {
        d.touched[d.touched_count++] = {address, address + bytes};
    } else {
        d.touched_overflow = true;
    }
}
} // namespace

void NoteDecodingCopy(std::uint64_t seq, const void* guest_dcb,
                      std::shared_ptr<const std::uint32_t> copy, std::uint64_t dwords) {
    std::scoped_lock lock{decoding_copy_mutex};
    DecodingCopy& d = decoding_copy;
    d.seq = seq;
    d.base = reinterpret_cast<std::uint64_t>(guest_dcb);
    d.dwords = copy ? dwords : 0;
    d.words = std::move(copy);
    d.touched_count = 0;
    d.touched_overflow = false;
    decoding_base.store(d.base, std::memory_order_relaxed);
    decoding_end.store(d.base + d.dwords * 4, std::memory_order_relaxed);
}

bool StaleLabel(std::uint64_t address, std::uint64_t value, std::uint32_t bytes, Source source) {
    (void)value;
    std::scoped_lock lock{decoding_copy_mutex};
    DecodingCopy& d = decoding_copy;
    const std::uint64_t end = d.base + d.dwords * 4;
    // Outside the dcb being decoded, or too many port writes into it to tell (then the label is
    // written, as before the guard).
    if (!d.words || d.touched_overflow || bytes == 0 || address < d.base || address + bytes > end ||
        (address & 3) != 0) {
        return false;
    }
    const std::uint32_t dwords = (bytes + 3) / 4;
    for (std::uint32_t i = 0; i < dwords; ++i) {
        const std::uint64_t at = address + i * 4;
        const std::uint32_t now = *reinterpret_cast<const volatile std::uint32_t*>(at);
        const std::uint32_t then = d.words.get()[(at - d.base) / 4];
        if (now == then) {
            continue;
        }
        bool port = false;
        for (std::size_t t = 0; t < d.touched_count && !port; ++t) {
            port = at >= d.touched[t].first && at < d.touched[t].second;
        }
        if (port) {
            continue; // the port itself changed it since the submit (an earlier label, WRITE_DATA)
        }
        const std::uint64_t n = stale_skipped.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 20 || n % 100 == 0) {
            std::fprintf(stderr,
                         "Issue #2 guard: skipped a late %s to %#llx (+%#llx of submission %llu's "
                         "command buffer): the game already reused it (now %08x, at submit %08x; "
                         "%llu skipped so far)\n",
                         SourceName(source), (unsigned long long)address,
                         (unsigned long long)(address - d.base), (unsigned long long)d.seq, now,
                         then, (unsigned long long)n);
        }
        return true;
    }
    return false;
}

void NoteWrite(std::uint64_t address, std::uint64_t value, std::uint32_t bytes, Source source) {
    if (bytes == 0 || address == 0) {
        return;
    }
    static thread_local const std::uint32_t tid = static_cast<std::uint32_t>(gettid());
    const std::uint64_t now = NowNs();
    const std::uint64_t decoding_now = decoding.load(std::memory_order_relaxed);
    writes[writes_head.fetch_add(1, std::memory_order_relaxed) % writes.size()] =
        Write{address, value, now, decoding_now, bytes, source, tid, 0};
    NoteTouched(address, bytes);
    // The newest submitted buffer the write lands in.
    const Range* hit = nullptr;
    const std::uint64_t head = ranges_head.load(std::memory_order_relaxed);
    const std::uint64_t count = std::min<std::uint64_t>(head, ranges.size());
    for (std::uint64_t i = 0; i < count; ++i) {
        const Range& r = ranges[(head - 1 - i) % ranges.size()];
        if (r.bytes != 0 && address < r.base + r.bytes && address + bytes > r.base &&
            (!hit || r.seq > hit->seq)) {
            hit = &r;
        }
    }
    if (!hit) {
        return;
    }
    const std::uint64_t n = hits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 24 && n % 500 != 0) {
        return;
    }
    const char* where = hit->seq == decoding_now ? "the buffer being decoded"
                        : hit->seq > decoding_now
                            ? "a buffer submitted after the one being decoded, not decoded yet"
                            : "an older buffer, not resubmitted since";
    const bool late = hit->seq > decoding_now;
    char thread[32] = "?";
    pthread_getname_np(pthread_self(), thread, sizeof(thread));
    std::fprintf(stderr,
                 "Guest write check: %s of %u bytes, value %#llx, to %#llx lands inside submission "
                 "%llu's command buffer at +%#llx (%s; decoding submission %llu; submitted %.2f ms "
                 "ago; thread %s; hit %llu)\n",
                 SourceName(source), bytes, (unsigned long long)value, (unsigned long long)address,
                 (unsigned long long)hit->seq, (unsigned long long)(address - hit->base), where,
                 (unsigned long long)decoding_now, (now - hit->ns) / 1e6, thread,
                 (unsigned long long)n);
    if (late) {
        AskForReport();
    }
}

void DumpWritesInto(std::uint64_t address, std::uint64_t bytes) {
    const std::uint64_t now = NowNs();
    const std::uint64_t head = writes_head.load(std::memory_order_relaxed);
    const std::uint64_t count = std::min<std::uint64_t>(head, writes.size());
    unsigned listed = 0;
    for (std::uint64_t i = 0; i < count && listed < 8; ++i) {
        const Write& w = writes[(head - 1 - i) % writes.size()];
        if (w.address < address + bytes && w.address + w.bytes > address) {
            std::fprintf(stderr,
                         "  port write into it: %s of %u bytes, value %#llx, to %#llx, %.2f ms ago, "
                         "while decoding submission %llu (thread %u)\n",
                         SourceName(w.source), w.bytes, (unsigned long long)w.value,
                         (unsigned long long)w.address, (now - w.ns) / 1e6,
                         (unsigned long long)w.decoding, w.tid);
            ++listed;
        }
    }
    if (listed == 0) {
        std::fprintf(stderr, "  no remembered port write into it (%llu writes remembered)\n",
                     (unsigned long long)count);
    }
}

void AskForReport() {
    static std::atomic<bool> asked{false};
    if (asked.exchange(true)) {
        return;
    }
    std::fflush(stdout);
    std::fprintf(stderr,
                 "\nSacredFire issue #2: the known \"clobbered dword\" bug just happened (a dword of a "
                 "submitted command buffer turned into 1); the lines above are its diagnostics.\n"
                 "SacredFire issue #2: please attach this whole log to "
                 "https://github.com/T3CHNOLOG1C/SacredFire/issues/2 (launcher: Log page, \"Export "
                 "log...\") so that the writer can be found and fixed.\n\n");
    std::fflush(stderr);
}

bool ProtectCopies() {
#if defined(_WIN32) || defined(__APPLE__)
    return false; // SacredFire (Windows, macOS): BB_COPY_PROTECT uses mprotect and SIGSEGV (Linux only)
#endif
    static const bool enabled = [] {
        const char* env = std::getenv("BB_COPY_PROTECT");
        return env && env[0] == '1';
    }();
    return enabled;
}

void* AllocCopy(std::size_t bytes) {
    if (!ProtectCopies()) {
        return std::malloc(bytes);
    }
#ifdef _WIN32
    return std::malloc(bytes);
#else
    return std::aligned_alloc(Page, RoundUp(bytes));
#endif
}

void FreeCopy(void* storage, std::size_t bytes) {
#ifndef _WIN32
    if (ProtectCopies()) {
        mprotect(storage, RoundUp(bytes), PROT_READ | PROT_WRITE);
        for (Protected& p : protected_copies) {
            if (p.base.load(std::memory_order_relaxed) == reinterpret_cast<std::uint64_t>(storage)) {
                p.base.store(0, std::memory_order_relaxed);
            }
        }
    }
#endif
    std::free(storage);
}

void ProtectCopy(std::uint64_t seq, const void* storage, std::size_t bytes) {
    if (!ProtectCopies() || bytes == 0) {
        return;
    }
    for (Protected& p : protected_copies) {
        std::uint64_t expected = 0;
        if (p.base.compare_exchange_strong(expected, reinterpret_cast<std::uint64_t>(storage),
                                           std::memory_order_relaxed)) {
            p.bytes.store(RoundUp(bytes), std::memory_order_relaxed);
            p.seq.store(seq, std::memory_order_relaxed);
            break;
        }
    }
#ifndef _WIN32
    if (mprotect(const_cast<void*>(storage), RoundUp(bytes), PROT_READ) != 0) {
        static std::atomic<bool> once{false};
        if (!once.exchange(true)) {
            std::perror("BB_COPY_PROTECT: mprotect");
        }
    }
#endif
}

bool OnFault(void* ucontext, std::uint64_t address) {
#if defined(_WIN32) || defined(__APPLE__)
    (void)ucontext;
    (void)address;
    return false;
#else
    if (!ProtectCopies()) {
        return false;
    }
    for (const Protected& p : protected_copies) {
        const std::uint64_t base = p.base.load(std::memory_order_relaxed);
        const std::uint64_t bytes = p.bytes.load(std::memory_order_relaxed);
        if (base != 0 && address >= base && address < base + bytes) {
            const auto* uc = static_cast<const ucontext_t*>(ucontext);
            char thread[32] = "?";
            pthread_getname_np(pthread_self(), thread, sizeof(thread));
            char line[256];
            const int n = std::snprintf(
                line, sizeof(line),
                "Clobber trap: a write to the read-only copy of submission %llu at +%#llx (host "
                "%#llx; copy of %llu bytes at %#llx) from RIP %#llx, thread %s\n",
                (unsigned long long)p.seq.load(std::memory_order_relaxed),
                (unsigned long long)(address - base), (unsigned long long)address,
                (unsigned long long)bytes, (unsigned long long)base,
                (unsigned long long)uc->uc_mcontext.gregs[REG_RIP], thread);
            if (n > 0) {
                const ssize_t written = write(2, line, std::min<std::size_t>(n, sizeof(line) - 1));
                (void)written;
            }
            AskForReport();
            return true;
        }
    }
    return false;
#endif
}
} // namespace BbClobber
