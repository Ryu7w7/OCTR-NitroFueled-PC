// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: GPU breadcrumbs, to name the command a GPU hang (ring timeout, then device lost) is
// stuck in. Each scheduler is a stream: its draws, dispatches and our own passes are numbered in
// recording order, and the GPU writes 2n before command n and 2n+1 after it into a host-visible
// buffer (VK_AMD_buffer_marker at the bottom of the pipe: written once all earlier work of the
// queue has finished). On device lost the last value says which command never finished, and the
// notes kept here say what it was. BB_BREADCRUMBS=0 turns them off.
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace Vulkan::Breadcrumbs {

enum class Kind : u8 {
    Pass,
    Draw,
    DrawIndexed,
    DrawIndirect,
    Dispatch,
    DispatchIndirect,
    Transfer,
    /// A pipeline barrier: size = image transitions, dst/src/hash[0]/hash[1] = the first four
    /// images, count = first image's old and new layout, 1 with a memory barrier.
    Barrier,
};

struct Crumb {
    Kind kind = Kind::Pass;
    const char* name = nullptr; ///< our passes and transfers: a string literal
    u64 hash[2]{};              ///< guest shaders: vertex and pixel, or compute
    u64 program[2]{};           ///< their code in guest memory
    u64 address = 0;            ///< indirect arguments; transfers: the guest address written
    u32 count[3]{};             ///< indices or vertices and instances, or groups; transfers:
                                ///< width, height and vk::Format of the image
    u32 args_slot = ~0u;        ///< indirect dispatches: where the GPU copies its arguments
    /// Transfers: the Vulkan handles written and read (images or buffers), the bytes, and the
    /// GPU address of a buffer written (0 when it has none).
    u64 dst = 0, src = 0, size = 0, dst_gpu = 0;
};

/// A Vulkan handle as a number, for Crumb::dst/src and NoteDestroyed.
template <typename Handle>
[[nodiscard]] inline u64 HandleId(Handle handle) noexcept {
    return u64(reinterpret_cast<uintptr_t>(static_cast<typename Handle::CType>(handle)));
}

/// An image or buffer destroyed (`what` a string literal): on device lost, a command that still
/// used it is named, and so is a fault address inside a freed buffer.
void NoteDestroyed(const char* what, u64 handle, u64 gpu_address, u64 size, u32 width = 0,
                   u32 height = 0, u32 format = 0);
/// A live image, so the report can describe the images of a barrier.
void NoteImageCreated(u64 handle, u32 width, u32 height, u32 format, u64 size);

/// A stream for a new scheduler (`name` a string literal); the first call creates the buffer.
u32 NewStream(const Instance& instance, const char* name);
[[nodiscard]] bool Enabled() noexcept;
/// Recording side, in command order: numbers the next command of `stream`.
u32 Note(u32 stream, const Crumb& crumb);
/// Recorded right before (`after` false) and right after the command numbered `id`.
void Mark(vk::CommandBuffer cmdbuf, u32 stream, u32 id, bool after);
/// Indirect dispatches: a slot of a host-visible buffer for the arguments the GPU reads (the
/// report and the statistics show them), ~0u when off. CopyArgs is recorded before the dispatch.
u32 ArgsSlot();
void CopyArgs(vk::CommandBuffer cmdbuf, vk::Buffer args, u64 offset, u32 slot);
/// Device lost: prints, per stream, the command the GPU did not finish and what it was.
void ReportDeviceLost(const char* where);
/// The same report, once, while the GPU has not finished submitted work for a long time (no
/// device lost yet: a reset may never come, or the waiting thread would not see it).
void ReportStuck(const char* where);
/// Frame statistics: per stream, the commands noted and how many the GPU has yet to finish.
void PrintProgress();

/// Breadcrumbs around what is recorded on `cmdbuf` right here (not deferred) in the scope.
class Scope {
public:
    Scope(vk::CommandBuffer cmdbuf_, u32 stream_, const char* name)
        : cmdbuf{cmdbuf_}, stream{stream_} {
        if (Enabled()) {
            id = Note(stream, {.name = name});
            Mark(cmdbuf, stream, id, false);
        }
    }
    ~Scope() {
        if (id) {
            Mark(cmdbuf, stream, id, true);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    vk::CommandBuffer cmdbuf;
    u32 stream;
    u32 id = 0;
};

} // namespace Vulkan::Breadcrumbs
