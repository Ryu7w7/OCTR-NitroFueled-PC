// SPDX-License-Identifier: GPL-2.0-or-later
// SacredFire issue #2, the clobbered dword: a single dword of a submitted command buffer (a PM4
// header, or a shader address register's payload) turns into 1. Always-on, cheap diagnostics
// that tell where the 1 arrives, and an opt-in trap for a host-side writer:
//
// - FirstBadHeader: the packet walk Liverpool::SubmitGfx runs over the copy it has just made
//   (= the guest buffer at submit) and ProcessGraphics runs again when it starts decoding it.
//   With the guest dword ReportBadPacket reads when it meets the bad header, that is three
//   readings per clobber: guest at submit, copy at decode, guest at decode.
// - NoteWrite: every write the port makes into guest memory on the game's behalf (fence labels,
//   WriteData, DMA, downloads) goes into a ring, and is reported at once when it lands inside
//   the range of a recently submitted command buffer (NoteSubmit). DumpWritesInto lists the
//   remembered writes into the clobbered dword.
// - BB_COPY_PROTECT=1: the host copies are allocated in whole pages and made read-only once
//   filled (ProtectCopy); a write into one faults, OnFault names the copy and the offset, and
//   the runtime's fault handler prints the writer's call chain and exits.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace BbClobber {
enum Source : std::uint32_t {
    Eop,
    Eos,
    EosGds,
    ReleaseMem,
    WriteData,
    ComputeWriteData,
    DmaFill,
    DmaCopy,
    BufferDownload,
    ImageDownload,
    ConstRam,
};

/// Dword offset of the first dword that is neither a type-3 packet header nor type-2 padding
/// when the buffer is walked packet by packet; -1 when the whole buffer parses.
std::int64_t FirstBadHeader(const std::uint32_t* words, std::uint64_t dwords);

/// Liverpool::SubmitGfx: a graphics submission's dcb in guest memory.
void NoteSubmit(std::uint64_t seq, const void* guest_dcb, std::uint64_t bytes);
/// GPU thread: the decoding of submission `seq` starts.
void NoteDecoding(std::uint64_t seq);

/// The guard against the clobber (the cause, seen on 2026-10-16 in a Windows log: "port write
/// into it: EOS fence ... while decoding submission S" with the clobbered submission S+2). The
/// game keeps its fence labels inside the two command buffers it alternates between, and may
/// start building frame S+2 into S's buffer while the port is still decoding S, which writes
/// S's labels: a label then lands on one of S+2's packet headers.
///
/// NoteDecodingCopy: the copy SubmitGfx made of submission `seq`'s dcb, kept alive here until
/// the next one. StaleLabel: before the port writes a fence label: true when the label lies in
/// that dcb and the game has already rewritten its slot (the guest dword matches neither the
/// copy taken at submit nor a label the port wrote there since), so the write must be skipped:
/// the game has moved on from that frame, and the slot is now one of its new commands.
void NoteDecodingCopy(std::uint64_t seq, const void* guest_dcb,
                      std::shared_ptr<const std::uint32_t> copy, std::uint64_t dwords);
bool StaleLabel(std::uint64_t address, std::uint64_t value, std::uint32_t bytes, Source source);
/// A write of `bytes` bytes to guest address `address` the port is about to make (`value`: the
/// first 8 bytes of what it writes). Reported when it lands inside a submitted command buffer.
void NoteWrite(std::uint64_t address, std::uint64_t value, std::uint32_t bytes, Source source);
/// The remembered writes that touched [address, address + bytes), newest first, on stderr.
void DumpWritesInto(std::uint64_t address, std::uint64_t bytes);

/// BB_COPY_PROTECT=1.
bool ProtectCopies();
/// Storage for a SubmittedCopy: whole pages, page-aligned, when ProtectCopies(); malloc otherwise.
void* AllocCopy(std::size_t bytes);
void FreeCopy(void* storage, std::size_t bytes);
/// After the copy is filled: read-only until FreeCopy (ProtectCopies() only).
void ProtectCopy(std::uint64_t seq, const void* storage, std::size_t bytes);
/// At a SIGSEGV: true when `address` is inside a protected copy (the write is reported; the
/// caller lets the fault be fatal so that the writer's call chain is printed).
bool OnFault(void* ucontext, std::uint64_t address);

/// The bug just showed itself (a bad header, a changed copy, a late label, a trap): once per
/// run, a message on stderr that names GitHub issue #2 and asks for the log to be attached
/// to it. The launcher watches for the words "SacredFire issue #2" and shows a notice.
/// BB_ISSUE2_TEST=1 prints it at the first submission (to try the launcher's notice).
void AskForReport();
} // namespace BbClobber
