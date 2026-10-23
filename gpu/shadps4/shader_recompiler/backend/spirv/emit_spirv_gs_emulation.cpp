// SPDX-License-Identifier: GPL-2.0-or-later

// bbport: geometry shaders without a geometry stage (MoltenVK). See runtime_info.h, GsEmulation,
// and video_core/renderer_vulkan/vk_gs_emulation.cpp for the two draws.
//
// Export stage: its outputs (position, then the ESGS ring vec4s) are stored per vertex.
// Geometry stage, as a vertex shader: invocation v is output vertex slot v % slots of primitive
// v / slots. The prologue loads the primitive's vertices (through the index buffer) into the
// values the geometry stage reads; EmitVertex returns when the slot's vertex is emitted, its
// outputs then written. A slot past the vertices emitted ends with position 0: its triangle is
// empty (a primitive's strips are whole triangles, three vertices each).

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {

struct Params {
    Id data_base;  ///< first vec4 of the draw's vertices
    Id entry_bias; ///< added to an index value (or primitive vertex number) for its entry
    Id index_size; ///< 0 (not indexed), 2, 4
    Id slots;      ///< output vertex slots per primitive
    Id index_address;
    Id stride; ///< vec4s per vertex
    Id es_bias; ///< added to the export stage's vertex index for its entry
    Id entries; ///< vertices stored for the draw
};

constexpr u32 ParamsStride = 48;

Id Address(EmitContext& ctx, u64 base, Id index, u32 stride) {
    return ctx.OpIAdd(ctx.U64, ctx.Constant(ctx.U64, base),
                      ctx.OpIMul(ctx.U64, ctx.OpUConvert(ctx.U64, index),
                                 ctx.Constant(ctx.U64, u64(stride))));
}

Params LoadParams(EmitContext& ctx) {
    const Id u32_type = ctx.U32[1];
    const Id param_ptr =
        ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, u32_type),
                          ctx.push_data_block, ctx.ConstU32(PushData::MotionParamIndex));
    const Id param_index = ctx.OpLoad(u32_type, param_ptr);
    const Id u32x4_ptr = ctx.TypePointer(spv::StorageClass::PhysicalStorageBuffer, ctx.U32[4]);
    const auto load = [&](u32 offset) {
        return ctx.OpLoad(ctx.U32[4],
                          ctx.OpConvertUToPtr(u32x4_ptr, Address(ctx, GsEmulation::params_address +
                                                                          offset,
                                                                 param_index, ParamsStride)),
                          spv::MemoryAccessMask::Aligned, 16u);
    };
    const Id p0 = load(0);
    const Id p1 = load(16);
    const Id p2 = load(32);
    const auto get = [&](Id vector, u32 component) {
        return ctx.OpCompositeExtract(u32_type, vector, component);
    };
    const Id index_low = ctx.OpUConvert(ctx.U64, get(p1, 0));
    const Id index_high = ctx.OpShiftLeftLogical(ctx.U64, ctx.OpUConvert(ctx.U64, get(p1, 1)),
                                                 ctx.ConstU32(32u));
    return {
        .data_base = get(p0, 0),
        .entry_bias = get(p0, 1),
        .index_size = get(p0, 2),
        .slots = get(p0, 3),
        .index_address = ctx.OpBitwiseOr(ctx.U64, index_low, index_high),
        .stride = get(p1, 2),
        .es_bias = get(p1, 3),
        .entries = get(p2, 0),
    };
}

/// The first vec4 of entry `entry` (clamped to the draw's vertices).
Id EntryBase(EmitContext& ctx, const Params& params, Id entry) {
    const Id u32_type = ctx.U32[1];
    const Id last = ctx.OpISub(u32_type, ctx.OpUMax(u32_type, params.entries, ctx.ConstU32(1u)),
                               ctx.ConstU32(1u));
    const Id clamped = ctx.OpUMin(u32_type, entry, last);
    return ctx.OpIAdd(u32_type, params.data_base, ctx.OpIMul(u32_type, clamped, params.stride));
}

Id DataPointer(EmitContext& ctx, Id vec4_index) {
    const Id f32x4_ptr = ctx.TypePointer(spv::StorageClass::PhysicalStorageBuffer, ctx.F32[4]);
    return ctx.OpConvertUToPtr(f32x4_ptr, Address(ctx, GsEmulation::data_address, vec4_index, 16));
}

Id LoadVec4(EmitContext& ctx, Id vec4_index) {
    return ctx.OpLoad(ctx.F32[4], DataPointer(ctx, vec4_index), spv::MemoryAccessMask::Aligned,
                      16u);
}

void StoreVec4(EmitContext& ctx, Id vec4_index, Id value) {
    ctx.OpStore(DataPointer(ctx, vec4_index), value, spv::MemoryAccessMask::Aligned, 16u);
}

/// An output variable of `count` floats, as a vec4 (missing components 0).
Id LoadOutput(EmitContext& ctx, Id variable, u32 count) {
    if (count == 4) {
        return ctx.OpLoad(ctx.F32[4], variable);
    }
    const Id value = ctx.OpLoad(ctx.F32[count], variable);
    std::array<Id, 4> parts{ctx.f32_zero_value, ctx.f32_zero_value, ctx.f32_zero_value,
                            ctx.f32_zero_value};
    for (u32 i = 0; i < count; ++i) {
        parts[i] = count == 1 ? value : ctx.OpCompositeExtract(ctx.F32[1], value, i);
    }
    return ctx.OpCompositeConstruct(ctx.F32[4], parts);
}

/// ESGS ring vec4s after the position (in_vertex_data_size, vertex_data_size: dwords).
u32 NumParams(u32 vertex_data_size) {
    return vertex_data_size / 4 > 0 ? vertex_data_size / 4 - 1 : 0;
}

} // Anonymous namespace

void EmitGsEmulationPrologue(EmitContext& ctx) {
    if (!ctx.GsEmulated()) {
        return;
    }
    const Id u32_type = ctx.U32[1];
    const Id bool_type = ctx.U1[1];
    const Params params = LoadParams(ctx);
    const Id vertex = ctx.OpLoad(u32_type, ctx.vertex_index);
    const Id primitive = ctx.OpUDiv(u32_type, vertex, params.slots);
    ctx.gs_emu_slot = ctx.OpUMod(u32_type, vertex, params.slots);
    ctx.OpStore(ctx.primitive_id, primitive);

    const Id is_32 = ctx.OpIEqual(bool_type, params.index_size, ctx.ConstU32(4u));
    const Id is_16 = ctx.OpIEqual(bool_type, params.index_size, ctx.ConstU32(2u));
    const Id indexed = ctx.OpLogicalOr(bool_type, is_32, is_16);
    const Id u32_ptr = ctx.TypePointer(spv::StorageClass::PhysicalStorageBuffer, u32_type);
    const u32 num_params = NumParams(ctx.runtime_info.hw.gs.in_vertex_data_size);
    std::array<std::array<Id, 3>, IR::NumParams> attributes{};
    for (u32 v = 0; v < 3; ++v) {
        const Id number = ctx.OpIAdd(u32_type, ctx.OpIMul(u32_type, primitive, ctx.ConstU32(3u)),
                                     ctx.ConstU32(v));
        // 16-bit indices are read as the aligned dword holding them. Without an index buffer the
        // load reads the parameters (a valid address) and is not used.
        const Id byte = ctx.OpSelect(u32_type, is_32,
                                     ctx.OpIMul(u32_type, number, ctx.ConstU32(4u)),
                                     ctx.OpIMul(u32_type, number, ctx.ConstU32(2u)));
        // The dword holding the index, from its absolute address (the index buffer may start
        // 2 bytes into one).
        const Id absolute =
            ctx.OpIAdd(ctx.U64, params.index_address, ctx.OpUConvert(ctx.U64, byte));
        const Id aligned = ctx.OpBitwiseAnd(ctx.U64, absolute, ctx.Constant(ctx.U64, ~u64{3}));
        const Id address = ctx.OpSelect(ctx.U64, indexed, aligned,
                                        ctx.Constant(ctx.U64, GsEmulation::params_address));
        const Id raw = ctx.OpLoad(u32_type, ctx.OpConvertUToPtr(u32_ptr, address),
                                  spv::MemoryAccessMask::Aligned, 4u);
        const Id low_bits = ctx.OpUConvert(u32_type, absolute);
        const Id shift = ctx.OpShiftLeftLogical(
            u32_type, ctx.OpBitwiseAnd(u32_type, low_bits, ctx.ConstU32(2u)), ctx.ConstU32(3u));
        const Id low = ctx.OpBitFieldUExtract(u32_type, raw, shift, ctx.ConstU32(16u));
        const Id value =
            ctx.OpSelect(u32_type, is_32, raw, ctx.OpSelect(u32_type, is_16, low, number));
        const Id base = EntryBase(ctx, params, ctx.OpIAdd(u32_type, value, params.entry_bias));
        ctx.gs_emu_positions[v] = LoadVec4(ctx, base);
        for (u32 p = 0; p < num_params; ++p) {
            attributes[p][v] = LoadVec4(ctx, ctx.OpIAdd(u32_type, base, ctx.ConstU32(p + 1)));
        }
    }
    for (u32 p = 0; p < num_params; ++p) {
        auto& param = ctx.input_params[p];
        param.id_array = attributes[p];
        param.pointer_type = ctx.input_f32;
        param.component_type = ctx.F32[1];
        param.num_components = 4;
        param.is_integer = false;
        param.is_loaded = true;
        param.is_array = true;
    }
}

void EmitGsEmulationVertex(EmitContext& ctx) {
    const Id u32_type = ctx.U32[1];
    const Id emitted = ctx.OpLoad(u32_type, ctx.gs_emu_counter);
    const Id found = ctx.OpIEqual(ctx.U1[1], emitted, ctx.gs_emu_slot);
    const Id return_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(found, return_label, merge_label);
    ctx.AddLabel(return_label);
    ctx.OpReturn();
    ctx.AddLabel(merge_label);
    ctx.OpStore(ctx.gs_emu_counter, ctx.OpIAdd(u32_type, emitted, ctx.ConstU32(1u)));
}

void EmitGsEmulationEpilogue(EmitContext& ctx) {
    if (ctx.GsEmulated()) {
        // The slot's vertex was never emitted.
        ctx.OpStore(ctx.output_position,
                    ctx.ConstantComposite(ctx.F32[4], ctx.f32_zero_value, ctx.f32_zero_value,
                                          ctx.f32_zero_value, ctx.f32_zero_value));
        return;
    }
    if (!ctx.EsEmulated()) {
        return;
    }
    const Id u32_type = ctx.U32[1];
    const Params params = LoadParams(ctx);
    const Id vertex = ctx.OpLoad(u32_type, ctx.vertex_index);
    const Id base = EntryBase(ctx, params, ctx.OpIAdd(u32_type, vertex, params.es_bias));
    StoreVec4(ctx, base, ctx.OpLoad(ctx.F32[4], ctx.output_position));
    const u32 num_params = NumParams(ctx.runtime_info.hw.es.vertex_data_size);
    for (u32 p = 0; p < num_params; ++p) {
        const auto& output = ctx.output_params[p];
        const Id value = Sirit::ValidId(output.id) ? LoadOutput(ctx, output.id, output.num_components)
                                                   : ctx.ConstantComposite(
                                                         ctx.F32[4], ctx.f32_zero_value,
                                                         ctx.f32_zero_value, ctx.f32_zero_value,
                                                         ctx.f32_zero_value);
        StoreVec4(ctx, ctx.OpIAdd(u32_type, base, ctx.ConstU32(p + 1)), value);
    }
}

} // namespace Shader::Backend::SPIRV
