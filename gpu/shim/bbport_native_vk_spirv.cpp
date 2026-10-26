// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_vk_spirv.h"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <unordered_map>

namespace BbNativeVk::Spirv {
namespace {
using u32 = std::uint32_t;

// opcodes
constexpr u32 OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25,
              OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeArray = 28,
              OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43,
              OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66,
              OpDecorate = 71, OpBitcast = 124;
// decorations
constexpr u32 DecBlock = 2, DecBufferBlock = 3, DecLocation = 30, DecBinding = 33,
              DecDescriptorSet = 34;
// storage classes
constexpr u32 ScUniformConstant = 0, ScInput = 1, ScUniform = 2, ScStorageBuffer = 12;

constexpr u32 Magic = 0x07230203;

u32 Opcode(u32 w) {
    return w & 0xffff;
}
u32 Count(u32 w) {
    return w >> 16;
}

/// Calls f(word index, opcode, word count) for every instruction; stops on a malformed one.
template <class F>
bool Walk(const std::vector<u32>& code, F&& f) {
    std::size_t i = 5;
    while (i < code.size()) {
        const u32 n = Count(code[i]);
        if (!n || i + n > code.size()) {
            return false;
        }
        f(i, Opcode(code[i]), n);
        i += n;
    }
    return true;
}
} // namespace

bool Prepare(std::vector<u32>& code, u32 set, std::vector<Binding>* bindings,
             u32* input_locations) {
    if (code.size() < 5 || code[0] != Magic) {
        return false;
    }
    std::unordered_map<u32, u32> binding_of, location_of, constants;
    std::unordered_map<u32, std::size_t> types; // result id -> word index
    std::unordered_map<u32, u32> block;         // struct id -> DecBlock / DecBufferBlock
    std::vector<std::size_t> variables;
    const bool ok = Walk(code, [&](std::size_t i, u32 op, u32 n) {
        if (op == OpDecorate && n >= 3) {
            const u32 id = code[i + 1], dec = code[i + 2];
            if (dec == DecDescriptorSet && n >= 4) {
                code[i + 3] = set;
            } else if (dec == DecBinding && n >= 4) {
                binding_of[id] = code[i + 3];
            } else if (dec == DecLocation && n >= 4) {
                location_of[id] = code[i + 3];
            } else if (dec == DecBlock || dec == DecBufferBlock) {
                block[id] = dec;
            }
        } else if (op >= OpTypeInt && op <= OpTypePointer && n >= 2) {
            types[code[i + 1]] = i;
        } else if (op == OpConstant && n >= 4) {
            constants[code[i + 2]] = code[i + 3];
        } else if (op == OpVariable && n >= 4) {
            variables.push_back(i);
        }
    });
    if (!ok) {
        return false;
    }
    bindings->clear();
    *input_locations = 0;
    for (const std::size_t v : variables) {
        const u32 ptr = code[v + 1], id = code[v + 2], storage = code[v + 3];
        if (storage == ScInput) {
            if (const auto l = location_of.find(id); l != location_of.end() && l->second < 32) {
                *input_locations |= 1u << l->second;
            }
            continue;
        }
        const auto b = binding_of.find(id);
        const auto p = types.find(ptr);
        if (b == binding_of.end() || p == types.end() || Opcode(code[p->second]) != OpTypePointer) {
            continue;
        }
        u32 type = code[p->second + 3], count = 1;
        for (auto t = types.find(type); t != types.end();) {
            const u32 op = Opcode(code[t->second]);
            if (op == OpTypeArray) {
                const auto c = constants.find(code[t->second + 3]);
                count *= c != constants.end() ? c->second : 1;
            } else if (op != OpTypeRuntimeArray) {
                break;
            }
            type = code[t->second + 2];
            t = types.find(type);
        }
        const auto t = types.find(type);
        if (t == types.end()) {
            continue;
        }
        const std::size_t ti = t->second;
        Binding out{b->second, Kind::UniformBuffer, 0, false, count};
        switch (Opcode(code[ti])) {
        case OpTypeStruct: {
            const auto k = block.find(type);
            if (storage == ScStorageBuffer || (k != block.end() && k->second == DecBufferBlock)) {
                out.kind = Kind::StorageBuffer;
            } else if (storage == ScUniform) {
                out.kind = Kind::UniformBuffer;
            } else {
                continue;
            }
            break;
        }
        case OpTypeImage: {
            // OpTypeImage id, sampled type, Dim, Depth, Arrayed, MS, Sampled, format
            out.dim = std::uint8_t(code[ti + 3]);
            out.arrayed = code[ti + 5] != 0;
            const bool storage_image = code[ti + 7] == 2;
            out.kind = out.dim == 5 ? (storage_image ? Kind::StorageTexelBuffer : Kind::UniformTexelBuffer)
                                    : (storage_image ? Kind::StorageImage : Kind::SampledImage);
            break;
        }
        case OpTypeSampler:
            out.kind = Kind::Sampler;
            break;
        case OpTypeSampledImage: // not made by DXC from HLSL; read as an image
            out.kind = Kind::SampledImage;
            out.dim = 1;
            break;
        default:
            continue;
        }
        (void)ScUniformConstant;
        bindings->push_back(out);
    }
    std::sort(bindings->begin(), bindings->end(),
              [](const Binding& a, const Binding& b) { return a.binding < b.binding; });
    return true;
}

bool IntegerInputs(std::vector<u32>& code, u32 uint_mask, u32 sint_mask) {
    if (!(uint_mask | sint_mask) || code.size() < 5 || code[0] != Magic) {
        return !(uint_mask | sint_mask);
    }
    std::unordered_map<u32, u32> location_of;
    std::unordered_map<u32, std::size_t> types;
    struct Input {
        std::size_t at;   // the OpVariable
        u32 id, components, float_vector, sign;
    };
    std::unordered_map<u32, Input> inputs; // variable id -> input to retype
    std::unordered_map<u32, bool> chained;  // inputs also read one component at a time
    std::size_t first_input = 0;
    Walk(code, [&](std::size_t i, u32 op, u32 n) {
        if (op == OpDecorate && n >= 4 && code[i + 2] == DecLocation) {
            location_of[code[i + 1]] = code[i + 3];
        } else if (op >= OpTypeInt && op <= OpTypePointer && n >= 2) {
            types[code[i + 1]] = i;
        }
    });
    Walk(code, [&](std::size_t i, u32 op, u32 n) {
        if (op == OpVariable && n >= 4 && code[i + 3] == ScInput) {
            const u32 id = code[i + 2];
            const auto l = location_of.find(id);
            if (l == location_of.end() || l->second >= 32) {
                return;
            }
            const u32 bit = 1u << l->second;
            if (!((uint_mask | sint_mask) & bit)) {
                return;
            }
            const auto p = types.find(code[i + 1]);
            if (p == types.end() || Opcode(code[p->second]) != OpTypePointer) {
                return;
            }
            const auto v = types.find(code[p->second + 3]);
            if (v == types.end() || Opcode(code[v->second]) != OpTypeVector) {
                return;
            }
            const auto f = types.find(code[v->second + 2]);
            if (f == types.end() || Opcode(code[f->second]) != OpTypeFloat || code[f->second + 2] != 32) {
                return;
            }
            inputs[id] = {i, id, code[v->second + 3], code[v->second + 1], (sint_mask & bit) ? 1u : 0u};
            if (!first_input || i < first_input) {
                first_input = i;
            }
        } else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 4 &&
                   inputs.count(code[i + 3])) {
            // a component of the input: OpAccessChain %ptr_Input_float %id %var %index
            chained[code[i + 3]] |= n != 5;
        }
    });
    for (const auto& [id, deep] : chained) {
        if (deep) {
            return false; // more than one index into a vector input: not something DXC makes
        }
    }
    if (inputs.empty()) {
        return true;
    }

    // the integer types needed, found or made; made ones go in front of the first input
    u32 bound = code[3];
    std::vector<u32> made;
    auto find = [&](u32 op, std::initializer_list<u32> operands) -> u32 {
        for (const auto& [id, at] : types) {
            if (Opcode(code[at]) != op || Count(code[at]) != operands.size() + 2) {
                continue;
            }
            if (std::equal(operands.begin(), operands.end(), code.begin() + at + 2)) {
                return at < first_input ? id : ~0u; // ~0: there, but after the inputs
            }
        }
        return 0;
    };
    bool order_ok = true;
    std::map<std::vector<u32>, u32> made_ids;
    auto get = [&](u32 op, std::initializer_list<u32> operands) -> u32 {
        std::vector<u32> key{op};
        key.insert(key.end(), operands.begin(), operands.end());
        if (const auto m = made_ids.find(key); m != made_ids.end()) {
            return m->second;
        }
        const u32 id = find(op, operands);
        if (id == ~0u) {
            order_ok = false;
            return 0;
        }
        if (id) {
            return id;
        }
        const u32 nid = made_ids[key] = bound++;
        made.push_back((u32(operands.size() + 2) << 16) | op);
        made.push_back(nid);
        made.insert(made.end(), operands.begin(), operands.end());
        return nid;
    };
    struct Retype {
        u32 pointer, vector, scalar_pointer, scalar;
    };
    std::unordered_map<u32, Retype> retype; // variable id ->
    for (const auto& [id, in] : inputs) {
        const u32 scalar = get(OpTypeInt, {32, in.sign});
        const u32 vector = get(OpTypeVector, {scalar, in.components});
        const u32 pointer = get(OpTypePointer, {ScInput, vector});
        const u32 scalar_pointer = chained.count(id) ? get(OpTypePointer, {ScInput, scalar}) : 0;
        retype[id] = {pointer, vector, scalar_pointer, scalar};
    }
    if (!order_ok) {
        return false;
    }

    std::unordered_map<u32, u32> components; // access chain id -> integer scalar type
    std::vector<u32> out(code.begin(), code.begin() + 5);
    out.reserve(code.size() + made.size() + 8 * inputs.size());
    Walk(code, [&](std::size_t i, u32 op, u32 n) {
        if (i == first_input) {
            out.insert(out.end(), made.begin(), made.end());
        }
        const std::size_t at = out.size();
        out.insert(out.end(), code.begin() + i, code.begin() + i + n);
        if (op == OpVariable) {
            if (const auto r = retype.find(code[i + 2]); r != retype.end()) {
                out[at + 1] = r->second.pointer;
            }
        } else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 4) {
            if (const auto r = retype.find(code[i + 3]); r != retype.end()) {
                out[at + 1] = r->second.scalar_pointer;
                components[code[i + 2]] = r->second.scalar;
            }
        } else if (op == OpLoad && n >= 4) {
            const auto r = retype.find(code[i + 3]);
            const auto c = components.find(code[i + 3]);
            if (r != retype.end() || c != components.end()) {
                const u32 float_type = code[i + 1], result = code[i + 2], raw = bound++;
                out[at + 1] = r != retype.end() ? r->second.vector : c->second;
                out[at + 2] = raw;
                out.insert(out.end(), {(4u << 16) | OpBitcast, float_type, result, raw});
            }
        }
    });
    out[3] = bound;
    code.swap(out);
    return true;
}
} // namespace BbNativeVk::Spirv
