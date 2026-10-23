// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/operand_helper.h"
#include "shader_recompiler/ir/position.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

void RingAccessElimination(const IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;

    const auto& ForEachInstruction = [&](auto func) {
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                func(ir, inst);
            }
        }
    };

    switch (program.info.hw_stage) {
    case HwStage::Local: {
        ForEachInstruction([=](IR::IREmitter& ir, IR::Inst& inst) {
            const auto opcode = inst.GetOpcode();
            switch (opcode) {
            case IR::Opcode::WriteSharedU64:
            case IR::Opcode::WriteSharedU32: {
                bool is_composite = opcode == IR::Opcode::WriteSharedU64;
                u32 num_components = opcode == IR::Opcode::WriteSharedU32 ? 1 : 2;

                ASSERT(inst.Arg(0).IsImmediate());

                u32 offset = inst.Arg(0).U32();
                IR::Value data =
                    is_composite ? ir.UnpackUint2x32(IR::U64{inst.Arg(1)}) : inst.Arg(1);
                for (s32 i = 0; i < num_components; i++) {
                    const auto attrib = IR::Attribute::Param0 + (offset / 16);
                    const auto comp = (offset / 4) % 4;
                    const IR::U32 value =
                        IR::U32{is_composite ? ir.CompositeExtract(data, i) : data};
                    ir.SetAttribute(attrib, ir.BitCast<IR::F32, IR::U32>(value), comp);
                    offset += 4;
                }
                inst.Invalidate();
                break;
            }
            default:
                break;
            }
        });
        break;
    }
    case HwStage::Export: {
        ForEachInstruction([=](IR::IREmitter& ir, IR::Inst& inst) {
            const auto opcode = inst.GetOpcode();
            switch (opcode) {
            case IR::Opcode::StoreBufferU32: {
                const auto info = inst.Flags<IR::BufferInstInfo>();
                if (!info.system_coherent || !info.globally_coherent) {
                    break;
                }

                const auto offset = inst.Flags<IR::BufferInstInfo>().inst_offset.Value();
                if (offset >= runtime_info.hw.es.vertex_data_size * 4) {
                    // bbport: a store past the vertex data the geometry stage reads (one
                    // racer's outfit online ended the game with code 23 here). Nothing reads
                    // it, so it is dropped instead of stopping the game.
                    LOG_WARNING(Render_Vulkan,
                                "RingAccessElimination: export store at byte {} is past the "
                                "vertex data ({} dwords); dropped",
                                offset, runtime_info.hw.es.vertex_data_size);
                    inst.Invalidate();
                    break;
                }
                const auto data = ir.BitCast<IR::F32>(IR::U32{inst.Arg(2)});
                const auto attrib =
                    IR::Value{offset < 16 ? IR::Attribute::Position0
                                          : IR::Attribute::Param0 + (offset / 16 - 1)};
                const u32 comp = (offset / 4) % 4;

                inst.ReplaceOpcode(IR::Opcode::SetAttribute);
                inst.ClearArgs();
                inst.SetArg(0, attrib);
                inst.SetArg(1, data);
                inst.SetArg(2, ir.Imm32(comp));
                break;
            }
            default:
                break;
            }
        });
        break;
    }
    case HwStage::Geometry: {
        const auto& gs_info = runtime_info.hw.gs;
        info.gs_copy_data = Shader::ParseCopyShader(gs_info.vs_copy);

        u32 dwords_per_vertex = gs_info.out_vertex_data_size;
        if (info.gs_copy_data.num_comps && info.gs_copy_data.num_comps > dwords_per_vertex) {
            LOG_WARNING(Render_Vulkan,
                        "VERT_ITEMSIZE {} is different than actual number of dwords per vertex {}",
                        dwords_per_vertex, info.gs_copy_data.num_comps);
            dwords_per_vertex = info.gs_copy_data.num_comps;
        }

        ForEachInstruction([&](IR::IREmitter& ir, IR::Inst& inst) {
            const auto opcode = inst.GetOpcode();
            switch (opcode) {
            case IR::Opcode::LoadBufferU32: {
                const auto info = inst.Flags<IR::BufferInstInfo>();
                if (!info.system_coherent || !info.globally_coherent) {
                    break;
                }

                auto vertex_id = (info.index_enable ? IR::GetBufferIndexArg(&inst)
                                                    : IR::GetBufferVOffsetArg(&inst))
                                     .U32() >>
                                 2;
                const auto soffset = IR::GetBufferSOffsetArg(&inst);
                auto bucket = soffset.U32() / 256u;
                // sacredfire: the ESGS ring stores input vertices consecutively, one component per
                // 256-byte slot and in_vertex_data_size components per vertex. CTR's fur geometry
                // shader addresses the second input vertex through the slot offset instead of the
                // vertex offset register; a slot past the first vertex belongs to the next one.
                // Without this the shader declared 15 input locations (over the 64-component
                // limit of this GPU) that its export stage never writes.
                const u32 in_vertex_dwords = gs_info.in_vertex_data_size;
                if (bucket >= 32u) {
                    LOG_WARNING(Render_Vulkan,
                                "sacredfire GS ring read: slot {} vertex {} (esgs itemsize {} dwords, "
                                "gsvs itemsize {}, copy comps {}, in_primitive {})",
                                bucket, vertex_id, in_vertex_dwords, gs_info.out_vertex_data_size,
                                program.info.gs_copy_data.num_comps, u32(gs_info.in_primitive));
                }
                if (in_vertex_dwords != 0 && bucket >= in_vertex_dwords) {
                    vertex_id += bucket / in_vertex_dwords;
                    bucket %= in_vertex_dwords;
                }
                const auto attrib = bucket < 4 ? IR::Attribute::Position0
                                               : IR::Attribute::Param0 + (bucket / 4 - 1);
                const auto comp = bucket % 4;

                auto attr_value = ir.GetAttribute(attrib, comp, vertex_id);
                inst.ReplaceOpcode(IR::Opcode::BitCastU32F32);
                inst.ClearArgs();
                inst.SetArg(0, attr_value);
                break;
            }
            case IR::Opcode::StoreBufferU32: {
                const auto buffer_info = inst.Flags<IR::BufferInstInfo>();
                if (!buffer_info.system_coherent || !buffer_info.globally_coherent) {
                    break;
                }

                const auto offset = inst.Flags<IR::BufferInstInfo>().inst_offset.Value();
                const auto data = ir.BitCast<IR::F32>(IR::U32{inst.Arg(2)});
                const auto comp_ofs = info.gs_copy_data.output_vertices * sizeof(u32);
                const auto output_size = gs_info.output_vertices * dwords_per_vertex * sizeof(u32);

                // bbport: a geometry stage whose ring registers are empty (max output vertices
                // 0, like the export stage's 0 dwords above) divided by zero here: online, a
                // racer's outfit ended the game with code 3. Its stores have nowhere to go,
                // so they are dropped, as the export stage's are.
                if (comp_ofs == 0 || output_size == 0) {
                    LOG_WARNING(Render_Vulkan,
                                "RingAccessElimination: geometry store at byte {} with no output "
                                "ring (max vertices {}, copy vertices {}, {} dwords); dropped",
                                offset, gs_info.output_vertices, info.gs_copy_data.output_vertices,
                                dwords_per_vertex);
                    inst.Invalidate();
                    break;
                }
                const auto vc_read_ofs = (((offset / comp_ofs) * comp_ofs) % output_size) * 16u;
                const auto& it = info.gs_copy_data.attr_map.find(vc_read_ofs);
                if (it == info.gs_copy_data.attr_map.cend()) {
                    LOG_WARNING(Render_Vulkan,
                                "RingAccessElimination: geometry store at byte {} has no copy "
                                "shader output (read offset {}); dropped",
                                offset, vc_read_ofs);
                    inst.Invalidate();
                    break;
                }
                const auto& [attr, comp] = it->second;

                inst.Invalidate();
                if (IsPosition(attr)) {
                    ExportPosition(ir, gs_info, false, attr, comp, data);
                } else {
                    ir.SetAttribute(attr, data, comp);
                }
                break;
            }
            default:
                break;
            }
        });
        break;
    }
    default:
        break;
    }
}

} // namespace Shader::Optimization
