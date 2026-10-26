// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: small SPIR-V reflection and patching for the native Vulkan renderer.
//
// The translated shaders (tools/shaders/build_spirv.py) put every stage's resources in descriptor
// set 0, as D3D11 gives each stage its own slots. Here each stage gets its own set (VS 0, PS 1,
// GS 2), and the bindings each one declares are read back to build its set layout.
//
// Their vertex inputs are all float4, and the shaders read integer data through asuint() (D3D11
// passes the raw bits). Vulkan wants the input's type to match the format, so for integer
// formats the input is retyped to uint4 / int4 and each load is bitcast back to float4.
#pragma once

#include <cstdint>
#include <vector>

namespace BbNativeVk::Spirv {
enum class Kind : std::uint8_t {
    UniformBuffer,
    StorageBuffer,
    SampledImage,
    StorageImage,
    Sampler,
    UniformTexelBuffer,
    StorageTexelBuffer,
};

struct Binding {
    std::uint32_t binding;
    Kind kind;
    std::uint8_t dim;  // SPIR-V Dim of an image: 0 1D, 1 2D, 2 3D, 3 cube, 5 buffer
    bool arrayed;      // an image array
    std::uint32_t count;
};

/// Reads the descriptor bindings and the input locations (bit n: Location n is an input) and
/// moves every descriptor to set `set`. False: not a SPIR-V module this can read.
bool Prepare(std::vector<std::uint32_t>& code, std::uint32_t set, std::vector<Binding>* bindings,
             std::uint32_t* input_locations);

/// Retypes the float vertex inputs at the locations in `uint_mask` to unsigned and in `sint_mask`
/// to signed integer vectors, bitcasting every load back. False: the module cannot be patched
/// (an input read through an access chain, or types out of order); `code` is then unchanged.
bool IntegerInputs(std::vector<std::uint32_t>& code, std::uint32_t uint_mask,
                   std::uint32_t sint_mask);
} // namespace BbNativeVk::Spirv
