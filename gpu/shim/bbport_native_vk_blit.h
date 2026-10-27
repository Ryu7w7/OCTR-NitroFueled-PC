// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "bbport_native_vk_gpu.h"

namespace Vulkan {
class Instance;
}

/// The native renderer's own draws (no engine shaders): copies that a transfer command cannot do.
namespace BbNativeVk::Blit {
void Init(const Vulkan::Instance& instance);

/// Copies texels of `src` (sampled through `src_view`; a depth image: its depth aspect) at `from`
/// into `rect` of the color image `dst`, as a draw in a pass on `dst`: no transfer, so the
/// targets stay compressed. Offsets count from the top. A color source needs the destination's
/// format, not an integer one (the caller checks); a depth source a float destination.
/// False: not done (nothing recorded).
bool Copy(Gpu::Image& src, VkImageView src_view, vk::Offset2D from, Gpu::Image& dst,
          vk::Rect2D rect);
} // namespace BbNativeVk::Blit
