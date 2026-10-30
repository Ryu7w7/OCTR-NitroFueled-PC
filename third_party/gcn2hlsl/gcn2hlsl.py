"""Ahead-of-time translator: PS4 GCN shader binaries -> HLSL (Shader Model 5) for D3D11.

Model: one HLSL invocation == one GCN lane. Every SGPR/VGPR is a `uint`, EXEC/VCC and SGPR
pairs used as lane masks are per-lane `bool`s. Control flow is rebuilt structurally from
the (structured) PSSL compiler output. Resources are bound from the Gnm input usage slots.

usage: python gcn2hlsl.py <shaders dir with index.json> <out dir> [name-filter]
"""
import ctypes
import json
import re
import os
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcn  # noqa: E402


class Unsupported(Exception):
    pass


# usage slot types (Gnm ShaderInputUsageType)
U_RES, U_SAMP, U_CB, U_VB = 0x00, 0x01, 0x02, 0x03
U_FETCH, U_RT, U_ST, U_CBT, U_VBT, U_EUD = 0x12, 0x13, 0x15, 0x16, 0x17, 0x1B
U_IGT = 0x1A  # PtrInternalGlobalTable: ES->GS and GS->VS ring descriptors

ES_VECS = 8  # ES->GS vertex: 32 dwords as float4 e[8]
GSO_LEN = 64  # GS output vertex dwords (GSVS ring attributes)
# D3D11 limit on geometry shader output: maxvertexcount * output scalars <= 1024
GS_OUT_SCALARS = 4 * (1 + len(range(15, 25)))

# interpolant semantics used by all VS exports / PS inputs (see export_shaders.py stats)
VARYINGS = range(15, 25)


def f32(x):
    return f"asfloat({x})"


class Translator:
    def __init__(self, kind, insts, meta, code_len, force_dims=None, ring=None, cube_slots=None):
        self.kind = kind
        # VS->GS pipeline: ring = {"role": "es"|"gs"|"copy", "max_vert": n} (GS ring addressing)
        self.ring = ring or {}
        self.insts = insts
        self.meta = meta
        self.code_len = code_len
        self.lines = []
        self.ind = 1
        self.sregs, self.vregs, self.masks = set(), set(), set()
        self.textures = {}  # slot -> (dim, kind)  kind: "float"
        self.samplers = {}  # slot -> "cmp"|"normal"
        self.cbuffers = set()
        self.inputs_used = {}  # attr index -> semantic, mode
        self.outputs = {}  # VS: sem -> True ; PS: mrt -> comps
        self.depth_out = False
        self.lane_spills = set()
        self.uses_quad = False
        self.uses_swizzle = False
        self.uses_vin = False  # vertex inputs converted per the runtime vertex format (b13)
        # render scale (supersampling): pixel positions / texel coordinates are converted between
        # the game's 1080p units and the scaled render targets (b13 ctr_scale, ctr_tscale)
        self.uses_scale = False
        self.cube_face_vgprs = set()  # VGPRs currently holding a v_cubeid_f32 result
        self.lane_addr_vgprs = set()  # VGPRs holding (lane id << k): per-lane LDS addresses
        self.lds_slots = set()  # byte offsets of per-lane LDS spill slots
        self.force_dims = force_dims or {}  # slot -> dims, when a slot is used with mixed dims
        # slots declared TextureCube (only plain cube lookups on them); what each slot was used for
        self.cube_slots = cube_slots or set()
        self.cube_used, self.cube_bad = set(), set()
        self.dims_seen = defaultdict(set)
        self.exec_written = any(self._writes_exec(i) for i in insts)
        self.idx = {i.pc: k for k, i in enumerate(insts)}
        self.tags = {}  # sgpr -> ("T"|"S"|"CB"|"V"|"P", slot/ptrkind, dword)
        self._init_tags()
        self._find_masks()

    # ------------------------------------------------------------ analysis
    @staticmethod
    def _writes_exec(i):
        if i.name.startswith("v_cmpx"):
            return True
        if i.name.endswith("saveexec_b64"):
            return True
        if i.fmt in ("SOP1", "SOP2", "SOPK") and i.f.get("sdst") == 126:
            return True
        return False

    def _init_tags(self):
        for s in self.meta.get("slots", []):
            u, api, reg, fl = s["usage"], s["api"], s["reg"], s["flags"]
            if reg >= 16:
                continue  # lives in extended user data, loaded via SMRD
            self._tag_slot(u, api, reg, fl)

    def _tag_slot(self, u, api, reg, fl):
        if u == U_RES:
            n = 8 if (fl & 1) else 4
            for k in range(n):
                self.tags[reg + k] = ("T" if n == 8 else "B", api, k)
        elif u == U_SAMP:
            for k in range(4):
                self.tags[reg + k] = ("S", api, k)
        elif u == U_CB:
            for k in range(4):
                self.tags[reg + k] = ("CB", api, k)
        elif u == U_VB:
            for k in range(4):
                self.tags[reg + k] = ("V", api, k)
        elif u in (U_FETCH, U_RT, U_ST, U_CBT, U_VBT, U_EUD, U_IGT):
            self.tags[reg] = ("P", u, 0)
            self.tags[reg + 1] = ("P", u, 1)

    def _find_masks(self):
        masks = set()
        changed = True
        while changed:
            changed = False
            for i in self.insts:
                d = None
                if i.name.endswith("saveexec_b64"):
                    d = i.f["sdst"]
                elif i.fmt == "VOP3" and i.f.get("base") == "VOPC" and i.f["sdst"] < 104:
                    d = i.f["sdst"]
                elif i.fmt in ("SOP1", "SOP2") and i.name.endswith("_b64") and i.f["sdst"] < 104:
                    ops = [i.f.get("ssrc0"), i.f.get("ssrc1")]
                    if any(o is not None and ((o.kind == "special" and o.value in ("exec_lo", "vcc_lo"))
                                              or (o.kind == "s" and o.value in masks)) for o in ops):
                        d = i.f["sdst"]
                if d is not None and d not in masks:
                    masks.add(d)
                    changed = True
        self.mask_pairs = masks

    # ------------------------------------------------------------ emit helpers
    _VCC_ASSIGN = re.compile(r"(?<![\w.])vcc = ")

    def emit(self, s):
        self.lines.append("    " * self.ind + s)
        # VCC doubles as a plain scalar register (vcc_lo used as a temp): keep its 32-bit
        # value `vccl` in sync whenever it is (re)defined as a lane mask
        if self._VCC_ASSIGN.search(s) and "vccl = " not in s:
            self.lines.append("    " * self.ind + "vccl = vcc ? 0xFFFFFFFFu : 0u;")

    def S(self, n):
        if n == 106:
            return "vcc_lo_"
        if n == 107:
            return "vcc_hi_"
        if n == 124:
            return "m0"
        if n == 126:
            return "exec_lo_"
        if n == 127:
            return "exec_hi_"
        if n >= 104:
            raise Unsupported(f"sgpr{n}")
        self.sregs.add(n)
        return f"s{n}"

    def V(self, n):
        self.vregs.add(n)
        return f"v{n}"

    def M(self, n):
        """lane-mask view of an SGPR pair / special"""
        if n == 106:
            return "vcc"
        if n == 126:
            return "ex"
        self.masks.add(n)
        return f"k{n}"

    def src(self, o):
        if o.kind == "s":
            return self.S(o.value)
        if o.kind == "v":
            return self.V(o.value)
        if o.kind == "lit":
            return f"0x{o.value:08x}u"
        if o.kind == "int":
            return f"{o.value & 0xFFFFFFFF}u"
        if o.kind == "float":
            return f"asuint({o.value!r}f)"
        v = o.value
        if v == "vcc_lo":
            return "vccl"
        if v == "exec_lo":
            return "(ex ? 0xFFFFFFFFu : 0u)"
        if v == "vcc_hi":
            return "vcch"
        if v == "exec_hi":
            return "0u"
        if v == "m0":
            return "m0"
        if v == "scc":
            return "(scc ? 1u : 0u)"
        if v == "vccz":
            return "(vcc ? 0u : 1u)"
        if v == "execz":
            return "(ex ? 0u : 1u)"
        raise Unsupported(f"operand {v}")

    def fsrc(self, i, n):
        """float source with VOP3 abs/neg modifiers"""
        o = i.f[f"src{n}"] if f"src{n}" in i.f else None
        if o is None:
            raise Unsupported("missing src")
        e = f32(self.src(o))
        if i.fmt == "VOP3":
            if (i.f.get("abs", 0) >> n) & 1:
                e = f"abs({e})"
            if (i.f.get("neg", 0) >> n) & 1:
                e = f"(-{e})"
        return e

    def vsrc1(self, i):
        return self.V(i.f["vsrc1"])

    def write_v(self, i, d, expr, is_float=True):
        if is_float and i.fmt == "VOP3":
            om = i.f.get("omod", 0)
            if om == 1:
                expr = f"(({expr}) * 2.0f)"
            elif om == 2:
                expr = f"(({expr}) * 4.0f)"
            elif om == 3:
                expr = f"(({expr}) * 0.5f)"
            if i.f.get("clamp"):
                expr = f"saturate({expr})"
        rhs = f"asuint({expr})" if is_float else expr
        guard = "if (ex) " if self.exec_written else ""
        self.emit(f"{guard}{self.V(d)} = {rhs};")
        if i is not None and i.name == "v_cubeid_f32":
            self.cube_face_vgprs.add(d)
        else:
            self.cube_face_vgprs.discard(d)
        # lane-id derived addresses (v_mbcnt, shifted): the shader keeps private spills in LDS
        srcs = [i.f.get(k) for k in ("src0", "src1")] if i is not None else []
        srcs = [o.value for o in srcs if o is not None and getattr(o, "kind", None) == "v"]
        if i is not None and "vsrc1" in i.f:
            srcs.append(i.f["vsrc1"])
        if i is not None and (i.name.startswith("v_mbcnt") or (
                i.name in ("v_lshlrev_b32", "v_lshl_b32", "v_mbcnt_hi_u32_b32")
                and any(r in self.lane_addr_vgprs for r in srcs))):
            self.lane_addr_vgprs.add(d)
        else:
            self.lane_addr_vgprs.discard(d)

    def write_s_scc(self, d, expr):
        """a SALU result that also sets SCC = (result != 0), whatever the destination"""
        if d >= 104:
            self.emit(f"scc = ({expr}) != 0u;")
            self.write_s(d, expr)
        else:
            self.write_s(d, expr)
            self.emit(f"scc = {self.S(d)} != 0u;")

    def write_s(self, d, expr):
        if d == 126:
            self.emit(f"ex = ({expr}) != 0u;")
        elif d == 106:
            self.emit(f"vccl = {expr}; vcc = vccl != 0u;")
        elif d == 107:
            self.emit(f"vcch = {expr};")
        elif d == 127:
            pass
        else:
            self.emit(f"{self.S(d)} = {expr};")
        if d < 104:
            self.tags.pop(d, None)

    def write_mask(self, d, expr):
        if d == 126:
            self.emit(f"ex = {expr};")
        elif d == 106:
            self.emit(f"vcc = {expr};")
        else:
            self.emit(f"{self.M(d)} = {expr};")

    def mask_of(self, o):
        if o.kind == "special":
            if o.value == "exec_lo":
                return "ex"
            if o.value == "vcc_lo":
                return "vcc"
            raise Unsupported(f"mask operand {o.value}")
        if o.kind == "s":
            if o.value in self.mask_pairs:
                return self.M(o.value)
            return f"({self.S(o.value)} != 0u)"
        if o.kind == "int":
            return "true" if o.value != 0 else "false"
        if o.kind == "lit":
            return "true" if o.value != 0 else "false"
        raise Unsupported("mask operand")

    # ------------------------------------------------------------ control flow
    def branch_target(self, i):
        off = (i.f["simm16"] ^ 0x8000) - 0x8000
        return i.pc + 4 + off * 4

    def cond(self, i):
        n = i.name
        return {"s_cbranch_scc0": "!scc", "s_cbranch_scc1": "scc", "s_cbranch_vccz": "!vcc",
                "s_cbranch_vccnz": "vcc", "s_cbranch_execz": "!ex",
                "s_cbranch_execnz": "ex", "s_branch": "true"}.get(n)

    def is_branch(self, i):
        return i.name == "s_branch" or (i.name.startswith("s_cbranch") and self.cond(i))

    def tidx(self, pc):
        if pc not in self.idx:
            if pc >= self.code_len:
                return len(self.insts)
            raise Unsupported("branch into instruction")
        return self.idx[pc]

    def loop_end_for(self, k):
        """index of the last backward branch to instruction k (loop header), or None"""
        last = None
        for j in range(k, len(self.insts)):
            i = self.insts[j]
            if self.is_branch(i) and self.tidx(self.branch_target(i)) == k:
                last = j
        return last

    def emit_range(self, a, b, loop=None):
        k = a
        while k < b:
            le = self.loop_end_for(k)
            if le is not None and le < b and (loop is None or k != loop[0]):
                # iteration cap: bad data must never hang the GPU (TDR); a GS cannot use more
                # iterations than it can emit vertices
                cap = self.ring.get("out_verts", 0) + 1 if self.kind == "gs" else 4096
                self.loop_id = getattr(self, "loop_id", 0) + 1
                self.emit(f"[loop] for (uint li{self.loop_id}_ = 0u; li{self.loop_id}_ < {cap}u; li{self.loop_id}_++) {{")
                self.ind += 1
                self.emit_range(k, le, (k, le + 1))
                br = self.insts[le]
                c = self.cond(br)
                if c != "true":
                    self.emit(f"if (!({c})) break;")
                self.ind -= 1
                self.emit("}")
                k = le + 1
                continue
            i = self.insts[k]
            if self.is_branch(i):
                t = self.tidx(self.branch_target(i))
                c = self.cond(i)
                if loop and t == loop[1]:
                    self.emit("break;" if c == "true" else f"if ({c}) break;")
                    k += 1
                    continue
                if loop and t == loop[0]:
                    self.emit("continue;" if c == "true" else f"if ({c}) continue;")
                    k += 1
                    continue
                if t <= k:
                    raise Unsupported("unstructured backward branch")
                if t > b:
                    if self.kind == "ps" and self.is_kill_block(t):
                        # whole-wave discard (no live lanes left): per lane this is a discard
                        self.emit(f"if ({c}) discard;" if c != "true" else "discard;")
                        k += 1
                        continue
                    if t >= len(self.insts) - 1 and self.insts[-1].name == "s_endpgm":
                        # early exit to the program end
                        self.emit_exit(c)
                        k += 1
                        continue
                    raise Unsupported("branch out of region")
                if c == "true":
                    raise Unsupported("unconditional forward jump")
                # if / else
                last = self.insts[t - 1] if t - 1 > k else None
                if last is not None and last.name == "s_branch":
                    e = self.tidx(self.branch_target(last))
                    if t < e <= b and not (loop and e == loop[1]):
                        self.emit(f"if (!({c})) {{")
                        self.ind += 1
                        self.emit_range(k + 1, t - 1, loop)
                        self.ind -= 1
                        self.emit("} else {")
                        self.ind += 1
                        self.emit_range(t, e, loop)
                        self.ind -= 1
                        self.emit("}")
                        k = e
                        continue
                # A GCN branch skips a block only when no lane needs it, and a pixel quad runs it
                # together (image samples take their LOD from all four): per lane, a quad
                # neighbour left out of the block gave AMD undefined derivatives (fur holes and
                # specks); the lanes that did not need it stay masked by `ex` inside.
                self.emit(f"if (quad_any(!({c}))) {{" if self.kind == "ps" else f"if (!({c})) {{")
                self.ind += 1
                self.emit_range(k + 1, t, loop)
                self.ind -= 1
                self.emit("}")
                k = t
                continue
            self.inst(i)
            k += 1

    def is_kill_block(self, t):
        """`s_mov_b64 exec, 0; exp null...; s_endpgm` tail emitted by the PSSL compiler for kill"""
        if t >= len(self.insts):
            return False
        i = self.insts[t]
        if i.name != "s_mov_b64" or i.f.get("sdst") != 126 or self.src(i.f["ssrc0"]) != "0u":
            return False
        for j in self.insts[t + 1:]:
            if j.name == "s_endpgm":
                return True
            if j.fmt != "EXP" or j.f.get("en", 0) != 0:
                return False
        return False

    def emit_exit(self, c):
        if self.kind == "gs":
            self.emit(f"if ({c}) return;" if c != "true" else "return;")
            return
        if self.kind == "ps":
            self.emit(f"if ({c}) {{ if (!ex || !done_) discard; return o; }}" if c != "true"
                      else "{ if (!ex || !done_) discard; return o; }")
        else:
            self.emit(f"if ({c}) return o;" if c != "true" else "return o;")

    # ------------------------------------------------------------ instructions
    def inst(self, i):
        n = i.name
        h = getattr(self, "op_" + n, None)
        if h:
            return h(i)
        if i.fmt in ("VOP1", "VOP2", "VOP3", "VOPC"):
            return self.valu(i)
        if i.fmt in ("SOP1", "SOP2", "SOPC", "SOPK"):
            return self.salu(i)
        if n in ("s_waitcnt", "s_nop", "s_dcache_inv", "s_sendmsg", "s_setprio", "s_barrier",
                 "s_icache_inv", "s_sleep", "s_trap"):
            return
        if n == "s_endpgm":
            if self.insts and i is not self.insts[-1]:
                self.emit_exit("true")
            return
        if i.fmt == "SMRD":
            return self.smrd(i)
        if i.fmt in ("MTBUF", "MUBUF"):
            t = self.tags.get(i.f.get("srsrc"))
            if t and t[0] == "RING":
                return self.ring_access(i)
            return self.buffer_load(i)
        if i.fmt == "MIMG":
            return self.mimg(i)
        if i.fmt == "EXP":
            return self.exp(i)
        if i.fmt == "VINTRP":
            return self.vintrp(i)
        if n in ("ds_write_b32", "ds_read_b32"):
            return self.ds_private(i)
        if n == "ds_swizzle_b32":
            return self.ds_swizzle(i)
        raise Unsupported(n)

    # ---- scalar
    def salu(self, i):
        n = i.name
        f = i.f
        if i.fmt == "SOPK":
            k = f["simm16"]
            ks = k - 0x10000 if k & 0x8000 else k
            d = f["sdst"]
            if n == "s_movk_i32":
                return self.write_s(d, f"{ks & 0xFFFFFFFF}u")
            if n == "s_addk_i32":
                return self.write_s(d, f"{self.S(d)} + {ks & 0xFFFFFFFF}u")
            if n == "s_mulk_i32":
                return self.write_s(d, f"uint(int({self.S(d)}) * {ks})")
            if n.startswith("s_cmpk_"):
                op = {"eq": "==", "lg": "!=", "gt": ">", "ge": ">=", "lt": "<", "le": "<="}[n.split("_")[2]]
                if n.endswith("i32"):
                    return self.emit(f"scc = int({self.S(d)}) {op} {ks};")
                return self.emit(f"scc = {self.S(d)} {op} {k}u;")
            raise Unsupported(n)
        if i.fmt == "SOPC":
            a, b = self.src(f["ssrc0"]), self.src(f["ssrc1"])
            parts = n.split("_")
            if parts[1] == "cmp":
                op = {"eq": "==", "lg": "!=", "gt": ">", "ge": ">=", "lt": "<", "le": "<="}[parts[2]]
                if parts[3] == "i32":
                    return self.emit(f"scc = int({a}) {op} int({b});")
                return self.emit(f"scc = {a} {op} {b};")
            if n == "s_bitcmp0_b32":
                return self.emit(f"scc = (({a} >> ({b} & 31u)) & 1u) == 0u;")
            if n == "s_bitcmp1_b32":
                return self.emit(f"scc = (({a} >> ({b} & 31u)) & 1u) != 0u;")
            raise Unsupported(n)
        d = f.get("sdst")
        if n.endswith("_b64") or n.endswith("_i64") or n.endswith("_u64"):
            return self.salu64(i)
        if i.fmt == "SOP1":
            a = self.src(f["ssrc0"])
            if n == "s_mov_b32":
                if f["ssrc0"].kind == "s" and f["ssrc0"].value in self.tags and d < 104:
                    self.tags[d] = self.tags[f["ssrc0"].value]
                    self.emit(f"{self.S(d)} = {a};")
                    return
                return self.write_s(d, a)
            if n == "s_not_b32":
                return self.write_s_scc(d, f"~{a}")
            if n == "s_brev_b32":
                return self.write_s(d, f"reversebits({a})")
            if n == "s_bcnt1_i32_b32":
                return self.write_s(d, f"countbits({a})")
            if n == "s_ff1_i32_b32":
                return self.write_s(d, f"firstbitlow({a})")
            if n == "s_flbit_i32_b32":
                return self.write_s(d, f"(firstbithigh({a}) == 0xFFFFFFFFu ? 0xFFFFFFFFu : 31u - firstbithigh({a}))")
            if n == "s_sext_i32_i8":
                return self.write_s(d, f"uint(int({a} << 24) >> 24)")
            if n == "s_sext_i32_i16":
                return self.write_s(d, f"uint(int({a} << 16) >> 16)")
            if n == "s_abs_i32":
                return self.write_s(d, f"uint(abs(int({a})))")
            if n == "s_cmov_b32":
                return self.emit(f"if (scc) {self.S(d)} = {a};")
            raise Unsupported(n)
        a, b = self.src(f["ssrc0"]), self.src(f["ssrc1"])
        ops = {"s_add_u32": ("+", True), "s_sub_u32": ("-", True), "s_add_i32": ("+", True),
               "s_sub_i32": ("-", True), "s_and_b32": ("&", False), "s_or_b32": ("|", False),
               "s_xor_b32": ("^", False), "s_mul_i32": ("*", None)}
        if n in ops:
            op, carry = ops[n]
            tmp = f"({a} {op} {b})"
            if n == "s_add_u32":
                self.emit(f"scc = ({a} + {b}) < {a};")
            elif n == "s_sub_u32":
                self.emit(f"scc = {b} > {a};")
            if carry is False:
                return self.write_s_scc(d, tmp)
            self.write_s(d, tmp)
            return
        if n in ("s_lshl_b32", "s_lshr_b32"):
            return self.write_s_scc(d, f"({a} {'<<' if 'lshl' in n else '>>'} ({b} & 31u))")
        if n == "s_ashr_i32":
            return self.write_s_scc(d, f"uint(int({a}) >> ({b} & 31u))")
        if n == "s_andn2_b32":
            return self.write_s_scc(d, f"({a} & ~{b})")
        if n == "s_orn2_b32":
            return self.write_s_scc(d, f"({a} | ~{b})")
        if n == "s_cselect_b32":
            return self.write_s(d, f"(scc ? {a} : {b})")
        if n in ("s_min_u32", "s_max_u32"):
            return self.write_s(d, f"{n[2:5]}({a}, {b})")
        if n in ("s_min_i32", "s_max_i32"):
            return self.write_s(d, f"uint({n[2:5]}(int({a}), int({b})))")
        if n == "s_bfm_b32":
            return self.write_s(d, f"(((1u << ({a} & 31u)) - 1u) << ({b} & 31u))")
        if n == "s_bfe_u32":
            return self.write_s(d, f"(({a} >> ({b} & 31u)) & ((1u << (({b} >> 16) & 127u)) - 1u))")
        if n == "s_bfe_i32":
            return self.write_s(d, f"uint(int({a} << (32u - (({b} >> 16) & 127u) - ({b} & 31u))) >> (32u - (({b} >> 16) & 127u)))")
        raise Unsupported(n)

    def salu64(self, i):
        n, f = i.name, i.f
        d = f["sdst"]
        is_mask = d in (106, 126) or d in self.mask_pairs
        if n == "s_mov_b64":
            if is_mask:
                return self.write_mask(d, self.mask_of(f["ssrc0"]))
            o = f["ssrc0"]
            if o.kind == "s":
                for k in range(2):
                    if o.value + k in self.tags:
                        self.tags[d + k] = self.tags[o.value + k]
                self.emit(f"{self.S(d)} = {self.S(o.value)}; {self.S(d + 1)} = {self.S(o.value + 1)};")
                return
            self.emit(f"{self.S(d)} = {self.src(o)}; {self.S(d + 1)} = 0u;")
            return
        if n == "s_wqm_b64":
            if d == 126:
                return  # whole quad mode: helper lanes, nothing to do per lane
            return self.write_mask(d, self.mask_of(f["ssrc0"]))
        if n.endswith("saveexec_b64"):
            a = self.mask_of(f["ssrc0"])
            op = n[2:].split("_saveexec")[0]
            expr = {"and": f"{a} && ex", "or": f"{a} || ex", "xor": f"{a} != ex",
                    "andn2": f"{a} && !ex", "orn2": f"{a} || !ex", "nand": f"!({a} && ex)",
                    "nor": f"!({a} || ex)", "xnor": f"{a} == ex"}[op]
            self.emit(f"{{ bool t_ = {expr}; {self.M(d)} = ex; ex = t_; scc = ex; }}")
            return
        if n == "s_not_b64":
            return self.write_mask(d, f"!({self.mask_of(f['ssrc0'])})")
        if i.fmt == "SOP2":
            a, b = self.mask_of(f["ssrc0"]), self.mask_of(f["ssrc1"])
            op = n[2:].split("_b64")[0]
            expr = {"and": f"({a} && {b})", "or": f"({a} || {b})", "xor": f"({a} != {b})",
                    "andn2": f"({a} && !{b})", "orn2": f"({a} || !{b})", "nand": f"!({a} && {b})",
                    "nor": f"!({a} || {b})", "xnor": f"({a} == {b})",
                    "cselect": f"(scc ? {a} : {b})"}.get(op)
            if expr is None:
                raise Unsupported(n)
            self.emit(f"{{ bool t_ = {expr}; {self.M(d) if d not in (106, 126) else ('vcc' if d == 106 else 'ex')} = t_; scc = t_; }}"
                      if op != "cselect" else "")
            if op == "cselect":
                self.write_mask(d, expr)
            return
        raise Unsupported(n)

    # ---- vector ALU
    def valu(self, i):
        n, f = i.name, i.f
        base = f.get("base", i.fmt)
        if base == "VOPC" or i.fmt == "VOPC":
            return self.vcmp(i)
        d = f["vdst"]
        a = self.fsrc(i, 0)
        ua = self.src(f["src0"])
        if i.fmt == "VOP3":
            b, c = (self.fsrc(i, 1), self.fsrc(i, 2))
            ub, uc = self.src(f["src1"]), self.src(f["src2"])
            if n in ("v_mac_f32", "v_mac_legacy_f32"):
                # VOP3-encoded v_mac: the addend is the destination, src2 is unused
                c, uc = f32(self.V(d)), self.V(d)
        elif i.fmt == "VOP2":
            b, c = f32(self.vsrc1(i)), f32(self.V(d))
            ub, uc = self.vsrc1(i), self.V(d)
        else:
            b = c = ub = uc = None
        F = lambda e: self.write_v(i, d, e, True)  # noqa: E731
        U = lambda e: self.write_v(i, d, e, False)  # noqa: E731
        fl = {
            "v_add_f32": lambda: F(f"{a} + {b}"), "v_sub_f32": lambda: F(f"{a} - {b}"),
            "v_subrev_f32": lambda: F(f"{b} - {a}"), "v_mul_f32": lambda: F(f"{a} * {b}"),
            "v_mul_legacy_f32": lambda: F(f"(({a}) == 0.0f || ({b}) == 0.0f ? 0.0f : {a} * {b})"),
            "v_mac_f32": lambda: F(f"mad({a}, {b}, {c})"),
            "v_mac_legacy_f32": lambda: F(f"mad({a}, {b}, {c})"),
            "v_mad_f32": lambda: F(f"mad({a}, {b}, {c})"),
            "v_mad_legacy_f32": lambda: F(f"mad({a}, {b}, {c})"),
            "v_fma_f32": lambda: F(f"mad({a}, {b}, {c})"),
            "v_madmk_f32": lambda: F(f"mad({a}, asfloat(0x{f['lit']:08x}u), {f32(self.vsrc1(i))})"),
            "v_madak_f32": lambda: F(f"mad({a}, {f32(self.vsrc1(i))}, asfloat(0x{f['lit']:08x}u))"),
            "v_min_f32": lambda: F(f"min({a}, {b})"), "v_max_f32": lambda: F(f"max({a}, {b})"),
            "v_min_legacy_f32": lambda: F(f"min({a}, {b})"),
            "v_max_legacy_f32": lambda: F(f"max({a}, {b})"),
            "v_min3_f32": lambda: F(f"min(min({a}, {b}), {c})"),
            "v_max3_f32": lambda: F(f"max(max({a}, {b}), {c})"),
            "v_med3_f32": lambda: F(f"max(min({a}, {b}), min(max({a}, {b}), {c}))"),
            "v_ldexp_f32": lambda: F(f"ldexp({a}, float(int({ub if i.fmt == 'VOP3' else self.vsrc1(i)})))"),
            "v_mov_b32": lambda: U(ua), "v_not_b32": lambda: U(f"~{ua}"),
            "v_fract_f32": lambda: F(f"frac({a})"), "v_trunc_f32": lambda: F(f"trunc({a})"),
            "v_ceil_f32": lambda: F(f"ceil({a})"), "v_floor_f32": lambda: F(self.floor_expr(i, a)),
            "v_rndne_f32": lambda: F(f"round({a})"), "v_exp_f32": lambda: F(f"exp2({a})"),
            "v_exp_legacy_f32": lambda: F(f"exp2({a})"),
            "v_log_f32": lambda: F(f"log2({a})"), "v_log_clamp_f32": lambda: F(f"log2({a})"),
            "v_log_legacy_f32": lambda: F(f"log2({a})"),
            "v_rcp_f32": lambda: F(f"rcp({a})"), "v_rcp_iflag_f32": lambda: F(f"rcp({a})"),
            "v_rcp_legacy_f32": lambda: F(f"rcp({a})"), "v_rcp_clamp_f32": lambda: F(f"rcp({a})"),
            "v_rsq_f32": lambda: F(f"rsqrt({a})"), "v_rsq_clamp_f32": lambda: F(f"min(rsqrt({a}), 3.402823e38f)"),
            "v_rsq_legacy_f32": lambda: F(f"rsqrt({a})"),
            "v_sqrt_f32": lambda: F(f"sqrt({a})"),
            "v_sin_f32": lambda: F(f"sin({a} * 6.28318548f)"),
            "v_cos_f32": lambda: F(f"cos({a} * 6.28318548f)"),
            "v_cvt_f32_i32": lambda: F(f"float(int({ua}))"), "v_cvt_f32_u32": lambda: F(f"float({ua})"),
            "v_cvt_i32_f32": lambda: U(f"uint(int({a}))"), "v_cvt_u32_f32": lambda: U(f"uint({a})"),
            "v_cvt_flr_i32_f32": lambda: U(f"uint(int(floor({a})))"),
            "v_cvt_rpi_i32_f32": lambda: U(f"uint(int(floor({a} + 0.5f)))"),
            "v_cvt_f16_f32": lambda: U(f"f32tof16({a})"),
            "v_cvt_f32_f16": lambda: F(f"f16tof32({ua})"),
            "v_cvt_f32_ubyte0": lambda: F(f"float({ua} & 0xFFu)"),
            "v_cvt_f32_ubyte1": lambda: F(f"float(({ua} >> 8) & 0xFFu)"),
            "v_cvt_f32_ubyte2": lambda: F(f"float(({ua} >> 16) & 0xFFu)"),
            "v_cvt_f32_ubyte3": lambda: F(f"float({ua} >> 24)"),
            "v_cvt_off_f32_i4": lambda: F(f"float(int({ua} << 28) >> 28) * 0.0625f"),
            "v_cvt_pkrtz_f16_f32": lambda: U(f"(f32tof16({a}) | (f32tof16({b}) << 16))"),
            "v_bfrev_b32": lambda: U(f"reversebits({ua})"),
            "v_ffbh_u32": lambda: U(f"(firstbithigh({ua}) == 0xFFFFFFFFu ? 0xFFFFFFFFu : 31u - firstbithigh({ua}))"),
            "v_ffbl_b32": lambda: U(f"firstbitlow({ua})"),
            "v_and_b32": lambda: U(f"({ua} & {ub})"), "v_or_b32": lambda: U(f"({ua} | {ub})"),
            "v_xor_b32": lambda: U(f"({ua} ^ {ub})"),
            "v_lshl_b32": lambda: U(f"({ua} << ({ub} & 31u))"),
            "v_lshr_b32": lambda: U(f"({ua} >> ({ub} & 31u))"),
            "v_lshlrev_b32": lambda: U(f"({ub} << ({ua} & 31u))"),
            "v_lshrrev_b32": lambda: U(f"({ub} >> ({ua} & 31u))"),
            "v_ashr_i32": lambda: U(f"uint(int({ua}) >> ({ub} & 31u))"),
            "v_ashrrev_i32": lambda: U(f"uint(int({ub}) >> ({ua} & 31u))"),
            "v_bfm_b32": lambda: U(f"(((1u << ({ua} & 31u)) - 1u) << ({ub} & 31u))"),
            "v_bcnt_u32_b32": lambda: U(f"(countbits({ua}) + {ub})"),
            "v_mbcnt_lo_u32_b32": lambda: U(f"{ub}"), "v_mbcnt_hi_u32_b32": lambda: U(f"{ub}"),
            "v_min_u32": lambda: U(f"min({ua}, {ub})"), "v_max_u32": lambda: U(f"max({ua}, {ub})"),
            "v_min_i32": lambda: U(f"uint(min(int({ua}), int({ub})))"),
            "v_max_i32": lambda: U(f"uint(max(int({ua}), int({ub})))"),
            "v_mul_u32_u24": lambda: U(f"(({ua} & 0xFFFFFFu) * ({ub} & 0xFFFFFFu))"),
            "v_mul_i32_i24": lambda: U(f"uint((int({ua} << 8) >> 8) * (int({ub} << 8) >> 8))"),
            "v_mad_u32_u24": lambda: U(f"(({ua} & 0xFFFFFFu) * ({ub} & 0xFFFFFFu) + {uc})"),
            "v_mad_i32_i24": lambda: U(f"uint((int({ua} << 8) >> 8) * (int({ub} << 8) >> 8) + int({uc}))"),
            "v_mul_lo_u32": lambda: U(f"({ua} * {ub})"), "v_mul_lo_i32": lambda: U(f"({ua} * {ub})"),
            "v_bfe_u32": lambda: U(f"(({ua} >> ({ub} & 31u)) & (({uc} & 31u) == 0u ? 0u : (0xFFFFFFFFu >> (32u - ({uc} & 31u)))))"),
            "v_bfe_i32": lambda: U(f"(({uc} & 31u) == 0u ? 0u : uint(int({ua} << (32u - ({uc} & 31u) - ({ub} & 31u))) >> (32u - ({uc} & 31u))))"),
            "v_bfi_b32": lambda: U(f"(({ua} & {ub}) | (~{ua} & {uc}))"),
            "v_alignbit_b32": lambda: U(f"(({uc} & 31u) == 0u ? {ub} : (({ub} >> ({uc} & 31u)) | ({ua} << (32u - ({uc} & 31u)))))"),
            "v_min3_i32": lambda: U(f"uint(min(min(int({ua}), int({ub})), int({uc})))"),
            "v_max3_i32": lambda: U(f"uint(max(max(int({ua}), int({ub})), int({uc})))"),
            "v_min3_u32": lambda: U(f"min(min({ua}, {ub}), {uc})"),
            "v_max3_u32": lambda: U(f"max(max({ua}, {ub}), {uc})"),
            "v_sad_u32": lambda: U(f"(({ua} > {ub} ? {ua} - {ub} : {ub} - {ua}) + {uc})"),
            "v_cubeid_f32": lambda: F(f"cubeid({a}, {b}, {c})"),
            "v_cubesc_f32": lambda: F(f"cubesc({a}, {b}, {c})"),
            "v_cubetc_f32": lambda: F(f"cubetc({a}, {b}, {c})"),
            "v_cubema_f32": lambda: F(f"cubema({a}, {b}, {c})"),
        }
        if n in fl:
            return fl[n]()
        if n == "v_cndmask_b32":
            m = "vcc" if i.fmt == "VOP2" else self.mask_of(f["src2"])
            mods = (f.get("abs", 0) | f.get("neg", 0)) if i.fmt == "VOP3" else 0
            x = f"asuint({self.fsrc(i, 0)})" if mods & 1 else ua  # -x / |x| as on any float source
            y = f"asuint({self.fsrc(i, 1)})" if mods & 2 else ub
            return U(f"({m} ? {y} : {x})")
        if n in ("v_add_i32", "v_sub_i32", "v_subrev_i32", "v_addc_u32", "v_subb_u32"):
            x, y = (ub, ua) if n == "v_subrev_i32" else (ua, ub)
            op = "+" if "add" in n else "-"
            cin = ""
            if n in ("v_addc_u32", "v_subb_u32"):
                cm = "vcc" if i.fmt == "VOP2" else self.mask_of(f["src2"])
                cin = f" {op} ({cm} ? 1u : 0u)"
            sd = 106 if i.fmt == "VOP2" else f["sdst"]
            res = f"({x} {op} {y}{cin})"
            carry = f"(({x} + {y}) < {x})" if op == "+" else f"({y} > {x})"
            if sd not in (106, 126) and sd not in self.mask_pairs:
                self.mask_pairs.add(sd)
            self.emit(f"{{ bool c_ = {carry}; {'if (ex) ' if self.exec_written else ''}{self.V(d)} = {res}; {self.M(sd) if sd != 106 else 'vcc'} = c_; }}")
            return
        if n == "v_readfirstlane_b32":
            return self.write_s(d, ua)
        if n == "v_readlane_b32":
            lane = f.get("vsrc1") if i.fmt == "VOP2" else None
            src_v = f["src0"].value if f["src0"].kind == "v" else None
            key = (src_v, lane)
            if key in self.lane_spills:
                return self.write_s(d, f"lane_v{src_v}_{lane}")
            return self.write_s(d, ua)
        if n == "v_writelane_b32":
            lane = f.get("vsrc1")
            self.lane_spills.add((d, lane))
            return self.emit(f"lane_v{d}_{lane} = {ua};")
        raise Unsupported(n)

    def vcmp(self, i):
        n, f = i.name, i.f
        parts = n.split("_")  # v_cmp[x]_<op>_<type>
        x = parts[1].endswith("x")
        op, ty = parts[2], parts[3]
        a = self.src(f["src0"])
        b = self.src(f["src1"]) if i.fmt == "VOP3" else self.V(f["vsrc1"])
        if ty == "f32":
            fa = self.fsrc(i, 0)
            fb = self.fsrc(i, 1) if i.fmt == "VOP3" else f32(b)
            if op == "class":
                expr = f"cmp_class({fa}, {b})"
            else:
                expr = {"f": "false", "lt": f"{fa} < {fb}", "eq": f"{fa} == {fb}", "le": f"{fa} <= {fb}",
                        "gt": f"{fa} > {fb}", "lg": f"{fa} != {fb}", "ge": f"{fa} >= {fb}",
                        "o": f"!(isnan({fa}) || isnan({fb}))", "u": f"(isnan({fa}) || isnan({fb}))",
                        "nge": f"!({fa} >= {fb})", "nlg": f"!({fa} != {fb})", "ngt": f"!({fa} > {fb})",
                        "nle": f"!({fa} <= {fb})", "neq": f"!({fa} == {fb})", "nlt": f"!({fa} < {fb})",
                        "tru": "true"}[op]
        elif ty in ("i32", "u32"):
            ca, cb = (f"int({a})", f"int({b})") if ty == "i32" else (a, b)
            expr = {"f": "false", "lt": f"{ca} < {cb}", "eq": f"{ca} == {cb}", "le": f"{ca} <= {cb}",
                    "gt": f"{ca} > {cb}", "ne": f"{ca} != {cb}", "ge": f"{ca} >= {cb}",
                    "t": "true"}[op]
        else:
            raise Unsupported(n)
        sd = 106 if i.fmt in ("VOPC",) else f["sdst"]
        guard = "ex && " if self.exec_written else ""
        if x:
            self.emit(f"{{ bool c_ = {guard}({expr}); ex = c_; {'vcc' if sd == 106 else self.M(sd)} = c_; }}")
        else:
            self.emit(f"{'vcc' if sd == 106 else self.M(sd)} = {guard}({expr});")

    # ---- memory: scalar loads (descriptors / constants)
    def smrd(self, i):
        n, f = i.name, i.f
        base, d = f["sbase"], f["sdst"]
        cnt = 1
        for k, c in (("x2", 2), ("x4", 4), ("x8", 8), ("x16", 16)):
            if n.endswith(k):
                cnt = c
        t = self.tags.get(base)
        if n.startswith("s_buffer_load"):
            if not t or t[0] not in ("CB", "B", "V"):
                raise Unsupported(f"s_buffer_load from untracked {t}")
            slot = t[1]
            self.cbuffers.add(slot)
            if f["imm"] or "lit" in f:
                # imm offsets and CI 32-bit literal offsets are in dwords
                off = f["offset"] if f["imm"] else f["lit"]
                for k in range(cnt):
                    dw = off + k
                    self.write_s(d + k, f"cb{slot}[{dw >> 2}].{'xyzw'[dw & 3]}")
            else:
                # SGPR offsets are in bytes
                o = self.S(f["offset"])
                for k in range(cnt):
                    self.write_s(d + k, f"cbload{slot}(({o} >> 2) + {k}u)")
            return
        if not t or t[0] != "P":
            raise Unsupported(f"s_load from untracked base {t}")
        ptr = t[1]
        if not f["imm"]:
            raise Unsupported("s_load with register offset")
        off = f["offset"]
        if ptr == U_EUD:
            # resolve which usage slot lives at extended user data dword `off`
            for s in self.meta.get("slots", []):
                if s["reg"] == 16 + off:
                    self._tag_slot(s["usage"], s["api"], d, s["flags"])
                    break
            else:
                raise Unsupported(f"extended user data offset {off}")
        elif ptr == U_RT:
            for k in range(cnt):
                self.tags[d + k] = ("T", off // 8, k)
        elif ptr == U_IGT:
            for k in range(cnt):
                self.tags[d + k] = ("RING", (off + k) // 4, (off + k) % 4)
        elif ptr == U_ST:
            for k in range(cnt):
                self.tags[d + k] = ("S", off // 4, k)
        elif ptr == U_CBT:
            for k in range(cnt):
                self.tags[d + k] = ("CB", off // 4, k)
        elif ptr == U_VBT:
            for k in range(cnt):
                self.tags[d + k] = ("V", off // 4, (off + k) % 4)
        else:
            raise Unsupported(f"s_load from pointer usage {ptr:#x}")
        for k in range(cnt):
            self.sregs.add(d + k)

    # ---- buffer loads (constant-buffer backed: e.g. bone palettes indexed per vertex)
    def ring_access(self, i):
        """ES->GS / GS->VS ring traffic of a geometry pipeline, mapped onto D3D11 GS I/O.
        ESGS ring: dword k of an ES vertex lives at byte k*256 (64 lanes); the GS addresses a
        vertex with its input offset (v0/v1/v3 << 2, set to the vertex index here).
        GSVS ring: GS writes dword (attr * max_vert + vertex) * 4; the copy VS reads attr at
        byte attr * max_vert * 64."""
        n, f = i.name, i.f
        if n not in ("buffer_load_dword", "buffer_store_dword"):
            raise Unsupported(f"{n} on a ring")
        role = self.ring.get("role")
        imm = f["offset"]
        va = self.V(f["vaddr"]) if f["offen"] else "0u"
        so = f["soffset"]
        soff = "0u" if so.kind == "int" and so.value == 0 else self.src(so)
        guard = "if (ex) " if self.exec_written else ""
        mv = self.ring.get("max_vert", 0)
        if n == "buffer_store_dword":
            v = self.V(f["vdata"])
            if role == "es":
                k = imm >> 2
                if f["offen"] or k >= 4 * ES_VECS:
                    raise Unsupported("ES ring store layout")
                return self.emit(f"{guard}o.e[{k >> 2}].{'xyzw'[k & 3]} = asfloat({v});")
            if role == "gs":
                # address = (attr * max_vert + vertex) * 4, the vertex (vaddr plus the vertex
                # part of the offset) below max_vert: the attribute is the offset's dword over
                # max_vert. A constant index keeps gso in registers (a computed one put it in
                # scratch memory: the fur GS ran 4x slower than on shadPS4)
                if mv and (imm >> 2) // mv < GSO_LEN:
                    return self.emit(f"{guard}gso[{(imm >> 2) // mv}u] = {v};")
                return self.emit(f"{guard}gso[min((({va} + {imm}u) >> 2) / {mv}u, {GSO_LEN - 1}u)] = {v};")
            raise Unsupported("ring store outside ES/GS")
        d = self.V(f["vdata"])
        if role == "gs":
            return self.emit(f"{{ uint vi_ = min({va} >> 2, 2u), k_ = ({soff} + {imm}u) >> 8; "
                             f"{guard}{d} = asuint(IN[vi_].e[k_ >> 2][k_ & 3u]); }}")
        if role == "copy":
            return self.emit(f"{guard}{d} = gso[min(({va} + {soff} + {imm}u) / {64 * mv}u, {GSO_LEN - 1}u)];")
        raise Unsupported("ring load outside GS/copy shader")

    def op_s_sendmsg(self, i):
        m = i.f["simm16"]
        msg, op = m & 0xF, (m >> 4) & 3
        if msg != 2 or self.kind != "gs":
            return  # GS_DONE, interrupts, ...
        guard = "if (ex) " if self.exec_written else ""
        if op in (2, 3):
            self.emit(f"{guard}{{ gsv = copyvs(gso); stream.Append(gsv); }}")
        if op in (1, 3):
            self.emit(f"{guard}stream.RestartStrip();")

    def buffer_load(self, i):
        n, f = i.name, i.f
        if "load" not in n:
            raise Unsupported(n)
        t = self.tags.get(f["srsrc"])
        if not t or t[0] != "CB":
            raise Unsupported(f"{n} from {t[0] if t else 'untracked'} descriptor")
        if i.fmt == "MTBUF":
            if f["dfmt"] not in (4, 11, 13, 14) or f["nfmt"] not in (4, 5, 7):
                raise Unsupported(f"tbuffer format {f['dfmt']}/{f['nfmt']}")
            count = {"x": 1, "xy": 2, "xyz": 3, "xyzw": 4}[n.rsplit("_", 1)[1]]
        elif n.startswith("buffer_load_dword"):
            count = {"buffer_load_dword": 1, "buffer_load_dwordx2": 2, "buffer_load_dwordx3": 3,
                     "buffer_load_dwordx4": 4}[n]
        elif n.startswith("buffer_load_format"):
            count = {"x": 1, "xy": 2, "xyz": 3, "xyzw": 4}[n.rsplit("_", 1)[1]]
        else:
            raise Unsupported(n)
        slot = t[1]
        self.cbuffers.add(slot)
        parts = [f"{f['offset']}u"]
        k = 0
        if f["idxen"]:
            parts.append(f"{self.V(f['vaddr'])} * 16u")  # constant buffer records are 16 bytes
            k = 1
        if f["offen"]:
            parts.append(self.V(f["vaddr"] + k))
        so = f["soffset"]
        if not (so.kind == "int" and so.value == 0):
            parts.append(self.src(so))
        byte = " + ".join(parts)
        self.emit(f"{{ uint b_ = ({byte}) >> 2;")
        for c in range(count):
            self.emit(f"    {'if (ex) ' if self.exec_written else ''}{self.V(f['vdata'] + c)} = cbload{slot}(b_ + {c}u);")
        self.emit("}")

    # ---- images
    def mimg(self, i):
        n, f = i.name, i.f
        rt = self.tags.get(f["srsrc"])
        if not rt or rt[0] != "T":
            raise Unsupported(f"{n} with untracked resource {rt}")
        slot = rt[1]
        a = f["vaddr"]
        av = lambda k: f32(self.V(a + k))  # noqa: E731
        dims = 3 if f["da"] else 2
        cube = False
        comps = [k for k in range(4) if (f["dmask"] >> k) & 1]
        dst = f["vdata"]

        def store(expr, ncomp=4, swizzle=True):
            if swizzle:
                # channel order of the bound view (RT component swap + T# dst_sel), supplied by the runtime
                expr = f"ctr_swizzle({expr}, ctr_swz[{slot >> 2}][{slot & 3}])"
                self.uses_swizzle = True
            self.emit(f"{{ float4 r_ = {expr};")
            for j, c in enumerate(comps):
                self.emit(f"    {'if (ex) ' if self.exec_written else ''}{self.V(dst + j)} = asuint(r_.{'xyzw'[c]});")
            self.emit("}")

        if n in ("image_load_mip", "image_load"):
            mip = f"int({self.V(a + dims)})" if n == "image_load_mip" else "0"
            ts = f"ctr_tscale[{slot >> 2}][{slot & 3}]"
            self.uses_scale = True
            coord = ", ".join(f"int(float(int({self.V(a + k)})) * {ts})" if k < 2 else f"int({self.V(a + k)})"
                              for k in range(dims))
            self.textures[slot] = (dims, "load")
            self.cube_bad.add(slot)
            return store(f"t{slot}.Load(int{dims + 1}({coord}, {mip}))")
        if n == "image_get_lod":
            ss = self.tags.get(f["ssamp"])
            if not ss or ss[0] != "S":
                raise Unsupported("get_lod sampler")
            self.samplers.setdefault(ss[1], "normal")
            self.textures[slot] = (dims, "sample")
            self.cube_bad.add(slot)
            c = f"float{dims}({', '.join(av(k) for k in range(dims))})"
            return store(f"float4(t{slot}.CalculateLevelOfDetail(sm{ss[1]}, {c}), "
                         f"t{slot}.CalculateLevelOfDetailUnclamped(sm{ss[1]}, {c}), 0, 0)", swizzle=False)
        if n == "image_get_resinfo":
            self.uses_scale = True
            self.textures[slot] = (dims, "load")
            self.cube_bad.add(slot)
            return store(f"texinfo{slot}(int({self.V(a)}))", swizzle=False)
        if not (n.startswith("image_sample") or n.startswith("image_gather4")):
            raise Unsupported(n)
        ss = self.tags.get(f["ssamp"])
        if not ss or ss[0] != "S":
            ss = self.loop_carried_sampler(f["ssamp"])
        if not ss or ss[0] != "S":
            raise Unsupported(f"{n} with untracked sampler {ss}")
        smp = ss[1]
        mods = n.split("image_sample")[-1] if n.startswith("image_sample") else n.split("image_gather4")[-1]
        k = 0
        offset = None
        if mods.endswith("_o"):
            offset = self.V(a + k)
            k += 1
            mods = mods[:-2]
        bias = None
        if "_b" in mods:
            bias = av(k)
            k += 1
        cmp_ = None
        if mods.startswith("_c"):
            cmp_ = av(k)
            k += 1
        grads = None
        if "_d" in mods or "_cd" in mods:
            grads = [av(k + j) for j in range(4)]
            k += 4
        if (a + k + 2) in self.cube_face_vgprs:
            # GCN cube lookup: (s, t) in [1, 2] + face index; the cube is bound as a 6-slice array
            dims, cube = 3, True
            coord = f"float3({av(k)} - 1.0f, {av(k + 1)} - 1.0f, {av(k + 2)})"
            if (grads is None and offset is None and cmp_ is None and not n.startswith("image_gather4")):
                self.cube_used.add(slot)
                if slot in self.cube_slots:  # a real cube: filtered across faces like the hardware
                    coord = f"cubedir({av(k)} - 1.0f, {av(k + 1)} - 1.0f, {av(k + 2)})"
            else:
                self.cube_bad.add(slot)
        else:
            self.cube_bad.add(slot)
            coord = f"float{dims}({', '.join(av(k + j) for j in range(dims))})"
        self.dims_seen[slot].add(dims)
        if self.force_dims.get(slot, dims) == 3 and dims == 2:
            coord = f"float3({coord}, 0.0f)"
            k += 2
            dims = 3
        else:
            k += dims
        lod = None
        if "_l" in mods and "_lz" not in mods:
            lod = av(k)
        if "_lz" in mods:
            lod = "0.0f"
        self.textures[slot] = (dims, "sample")
        tex = f"t{slot}"
        if cmp_ is not None:
            if self.samplers.get(smp) == "normal":
                raise Unsupported("sampler used as both compare and normal")
            self.samplers[smp] = "cmp"
        else:
            if self.samplers.get(smp) == "cmp":
                raise Unsupported("sampler used as both compare and normal")
            self.samplers[smp] = "normal"
        sm = f"sm{smp}"
        if offset is not None:
            coord = f"({coord} + texoff{dims}({tex}, {offset}))"
        if n.startswith("image_gather4"):
            ch = "Red" if not comps else ["Red", "Green", "Blue", "Alpha"][comps[0]]
            comps[:] = [0, 1, 2, 3]
            if cmp_ is not None:
                return store(f"{tex}.GatherCmp{ch}({sm}, {coord}, {cmp_})", swizzle=False)
            return store(f"{tex}.Gather{ch}({sm}, {coord})", swizzle=False)
        if cmp_ is not None:
            if lod is not None and lod != "0.0f":
                raise Unsupported("compare with explicit lod")
            fn = "SampleCmpLevelZero" if lod is not None or self.kind != "ps" else "SampleCmp"
            return store(f"{tex}.{fn}({sm}, {coord}, {cmp_}).xxxx", swizzle=False)
        if grads is not None:
            gx = f"float{dims}({grads[0]}, {grads[1]}{', 0' if dims == 3 else ''})"
            gy = f"float{dims}({grads[2]}, {grads[3]}{', 0' if dims == 3 else ''})"
            return store(f"{tex}.SampleGrad({sm}, {coord}, {gx}, {gy})")
        if lod is not None:
            return store(f"{tex}.SampleLevel({sm}, {coord}, {lod})")
        if self.kind != "ps":
            return store(f"{tex}.SampleLevel({sm}, {coord}, 0)")
        if bias is not None:
            return store(f"{tex}.SampleBias({sm}, {coord}, {bias})")
        return store(f"{tex}.Sample({sm}, {coord})")

    # ---- interpolation / exports
    def vintrp(self, i):
        f = i.f
        attr = f["attr"]
        ins = self.meta.get("inputs", [])
        if attr >= len(ins):
            raise Unsupported("interp attr out of range")
        sem = ins[attr]["sem"]
        mode = "flat" if i.name == "v_interp_mov_f32" or ins[attr]["flat"] else (
            "linear" if ins[attr]["linear"] else "persp")
        self.inputs_used[sem] = mode
        if i.name == "v_interp_p1_f32":
            return
        if i.name == "v_interp_mov_f32":
            # P0/P10/P20 selection is approximated by the flat value
            pass
        self.write_v(i, f["vdst"], f"IN.a{sem}.{'xyzw'[f['chan']]}", True)

    def exp(self, i):
        f = i.f
        tgt, en, src = f["tgt"], f["en"], f["vsrc"]
        if tgt == 9:  # null export
            if self.kind == "ps" and f["done"]:
                self.emit("if (!ex) discard;")
                self.emit("done_ = true;")
            return
        if self.kind in ("es", "gs"):
            raise Unsupported(f"{self.kind} export")
        if self.kind == "vs":
            if tgt == 12:
                vals = [f32(self.V(src[k])) if (en >> k) & 1 else ("1.0f" if k == 3 else "0.0f")
                        for k in range(4)]
                self.emit(f"o.pos = float4({', '.join(vals)});")
                return
            if 13 <= tgt <= 15:
                return  # misc / clip distances
            if 32 <= tgt < 64:
                p = tgt - 32
                sem = next((e["sem"] for e in self.meta.get("exports", []) if e["out"] == p), None)
                if sem is None:
                    return
                self.outputs[sem] = True
                vals = [f32(self.V(src[k])) if (en >> k) & 1 else "0.0f" for k in range(4)]
                self.emit(f"o.a{sem} = float4({', '.join(vals)});")
                return
            raise Unsupported(f"vs export target {tgt}")
        # pixel shader
        if tgt == 8:
            if en & 1:
                self.depth_out = True
                self.emit(f"o.depth = {f32(self.V(src[0]))};")
            if f["done"]:
                self.emit("if (!ex) discard;")
                self.emit("done_ = true;")
            return
        if tgt < 8:
            self.outputs[tgt] = True
            if f["compr"]:
                v0, v1 = self.V(src[0]), self.V(src[1])
                self.emit(f"o.c{tgt} = float4(f16tof32({v0} & 0xFFFFu), f16tof32({v0} >> 16), "
                          f"f16tof32({v1} & 0xFFFFu), f16tof32({v1} >> 16));")
            else:
                vals = [f32(self.V(src[k])) if (en >> k) & 1 else "0.0f" for k in range(4)]
                self.emit(f"o.c{tgt} = float4({', '.join(vals)});")
            if f["done"]:
                self.emit("if (!ex) discard;")
                self.emit("done_ = true;")
            return
        raise Unsupported(f"ps export target {tgt}")

    def loop_carried_sampler(self, reg):
        """an S# loaded later in program order (inside a loop, used on the next iteration)"""
        for i in self.insts:
            if i.name == "s_load_dwordx4" and i.f["sdst"] == reg and i.f["imm"]:
                t = self.tags.get(i.f["sbase"])
                if t and t[0] == "P" and t[1] == U_ST:
                    return ("S", i.f["offset"] // 4, 0)
        return None

    def ds_private(self, i):
        """LDS addressed by lane id: per-lane private storage (register spills)"""
        f = i.f
        if f["gds"] or f["addr"] not in self.lane_addr_vgprs:
            raise Unsupported(f"{i.name} with shared address")
        off = f["offset0"] | (f["offset1"] << 8)
        self.lds_slots.add(off)
        guard = "if (ex) " if self.exec_written else ""
        if i.name == "ds_write_b32":
            return self.emit(f"{guard}lds{off} = {self.V(f['data0'])};")
        return self.emit(f"{guard}{self.V(f['vdst'])} = lds{off};")

    def ds_swizzle(self, i):
        if self.kind != "ps":
            raise Unsupported("ds_swizzle outside pixel shader")
        off = i.f["offset0"] | (i.f["offset1"] << 8)
        if not off & 0x8000:
            raise Unsupported("ds_swizzle bitmask mode")
        perm = off & 0xFF
        self.uses_quad = True
        self.write_v(i, i.f["vdst"], f"quad_swizzle({f32(self.V(i.f['addr']))}, {perm}u, qlane)", True)

    def op_s_swappc_b64(self, i):
        if self.kind not in ("vs", "es"):
            raise Unsupported("s_swappc outside vertex shader")
        self.uses_vin = True
        for inp in self.meta.get("inputs", []):
            for c in range(inp["size"]):
                self.emit(f"{self.V(inp['vgpr'] + c)} = ctr_in(IN.a{inp['sem']}.{'xyzw'[c]}, {inp['sem']}u);")

    def op_s_cbranch_join(self, i):
        raise Unsupported("s_cbranch_join")

    # ------------------------------------------------------------ program
    def translate(self):
        body_start = len(self.lines)
        self.emit_range(0, len(self.insts))
        body = self.lines[body_start:]
        self.lines = self.lines[:body_start]
        return self.assemble(body)

    def assemble(self, body):
        pin = self.ps_input_vgprs() if self.kind == "ps" else []  # sets uses_scale before the decls
        out = ["// translated from PS4 GCN by tools/gcn2hlsl.py", COMMON_HLSL]
        for slot in sorted(self.cbuffers):
            out.append(f"cbuffer CB{slot} : register(b{slot}) {{ uint4 cb{slot}[4096]; }};")
            out.append(f"uint cbload{slot}(uint dw) {{ return cb{slot}[dw >> 2][dw & 3u]; }}")
        for slot, (dims, use) in sorted(self.textures.items()):
            ty = "TextureCube" if slot in self.cube_slots else "Texture2D" if dims == 2 else "Texture2DArray"
            out.append(f"{ty}<float4> t{slot} : register(t{slot});")
            if use == "load" or any("texinfo" in l for l in body):
                pass
        if self.uses_swizzle or self.uses_vin or self.uses_scale:
            out.append("cbuffer CTRSwizzle : register(b13) { uint4 ctr_swz[8]; uint4 ctr_vin[8]; "
                       "float4 ctr_scale; float4 ctr_tscale[8]; };")
        if self.uses_vin:
            out.append("uint ctr_in(float x, uint sem) { uint m = ctr_vin[sem >> 2][sem & 3u]; uint u = asuint(x); "
                       "return m == 1u ? asuint(float(u)) : m == 2u ? asuint(float(int(u))) : u; }")
        if self.uses_swizzle:
            out.append("float ctr_sel(float4 v, uint c) { return c == 0u ? 0.0f : c == 1u ? 1.0f : v[c - 2u]; }")
            out.append("float4 ctr_swizzle(float4 v, uint s) { return s == 0u ? v : float4(ctr_sel(v, s & 7u), "
                       "ctr_sel(v, (s >> 3) & 7u), ctr_sel(v, (s >> 6) & 7u), ctr_sel(v, (s >> 9) & 7u)); }")
        for slot, kind in sorted(self.samplers.items()):
            ty = "SamplerComparisonState" if kind == "cmp" else "SamplerState"
            out.append(f"{ty} sm{slot} : register(s{slot});")
        if any(f"texinfo{s}(" in l for l in body for s in self.textures):
            for slot, (dims, _u) in self.textures.items():
                out.append(f"float4 texinfo{slot}(int mip) {{ uint w, h, d, l; "
                           + ("t{0}.GetDimensions(mip, w, h, l); d = {1};".format(slot, 6 if slot in self.cube_slots else 1)
                              if dims == 2 or slot in self.cube_slots else
                              "t{0}.GetDimensions(mip, w, h, d, l);".format(slot))
                           + f" float s_ = ctr_tscale[{slot >> 2}][{slot & 3}]; w = uint(w / s_ + 0.5f); "
                           "h = uint(h / s_ + 0.5f); return asfloat(uint4(w, h, d, l)); }")
        for dims in (2, 3):
            out.append(f"float{dims} texoff{dims}(Texture2D{'Array' if dims == 3 else ''}<float4> t, uint o) {{ uint w, h{', d' if dims == 3 else ''}, l; "
                       f"t.GetDimensions(0, w, h{', d' if dims == 3 else ''}, l); "
                       f"int2 off = int2(int(o << 26) >> 26, int(o << 18) >> 26); "
                       f"return float{dims}(off.x / float(w), off.y / float(h){', 0' if dims == 3 else ''}); }}")
        # I/O structs
        if self.kind in ("vs", "es"):
            out.append("struct VSIn {")
            for inp in self.meta.get("inputs", []):
                out.append(f"    float4 a{inp['sem']} : ATTRIB{inp['sem']};")
            out.append("    uint vid : SV_VertexID;\n    uint iid : SV_InstanceID;\n};")
            if self.kind == "vs":
                out.append(VSOUT_STRUCT)
                out.append("VSOut main(VSIn IN) {\n    VSOut o = (VSOut)0;")
            else:
                out.append(ESOUT_STRUCT)
                out.append("ESOut main(VSIn IN) {\n    ESOut o = (ESOut)0;")
        elif self.kind == "gs":
            out.append(ESOUT_STRUCT)
            out.append(VSOUT_STRUCT)
            out.append(self.ring["copy_fn"])
            out.append(f"[maxvertexcount({self.ring['out_verts']})]")
            out.append("void main(triangle ESOut IN[3], uint pid : SV_PrimitiveID, "
                       "inout TriangleStream<VSOut> stream) {")
            out.append(f"    uint gso[{GSO_LEN}] = {{ {', '.join(['0u'] * GSO_LEN)} }};")
            # fxc wants the emitted struct initialised on every path (X4580)
            out.append("    VSOut gsv = (VSOut)0;")
        else:
            out.append("struct PSIn {\n    float4 pos : SV_Position;")
            for s in VARYINGS:
                mode = self.inputs_used.get(s, "persp")
                q = {"flat": "nointerpolation ", "linear": "noperspective ", "persp": ""}[mode]
                out.append(f"    {q}float4 a{s} : VARYING{s};")
            out.append("    bool ff : SV_IsFrontFace;\n};")
            out.append("struct PSOut {")
            for t in sorted(k for k in self.outputs):
                out.append(f"    float4 c{t} : SV_Target{t};")
            if self.depth_out:
                out.append("    float depth : SV_Depth;")
            if not self.outputs and not self.depth_out:
                out.append("    float4 c0 : SV_Target0;")
            out.append("};")
            out.append("PSOut main(PSIn IN) {\n    PSOut o = (PSOut)0;")
        out += self.decls()
        if self.kind in ("vs", "es"):
            out.append("    v0 = IN.vid;" if 0 in self.vregs else "")
            out.append("    v3 = IN.iid;" if 3 in self.vregs and ((self.meta['regs']['rsrc1'] >> 24) & 3) == 3 else "")
        elif self.kind == "gs":
            # vertex offsets into the ES->GS ring stand for the input vertex index
            for r, val in ((0, "0u"), (1, "1u"), (2, "pid"), (3, "2u")):
                if r in self.vregs:
                    out.append(f"    v{r} = {val};")
        else:
            out += pin
            if self.uses_quad:
                out.append("    uint qlane = (uint(IN.pos.x) & 1u) | ((uint(IN.pos.y) & 1u) << 1);")
        out += body
        if self.kind == "ps":
            out.append("    if (!done_) discard;")
        out.append("    return o;\n}" if self.kind != "gs" else "}")
        return "\n".join(x for x in out if x != "")

    def function_text(self, header):
        """the translated program as a helper function (GS copy shader)"""
        body_start = len(self.lines)
        self.emit_range(0, len(self.insts))
        body = self.lines[body_start:]
        self.lines = self.lines[:body_start]
        out = [header + " {", "    VSOut o = (VSOut)0;"] + self.decls()
        out.append("    v0 = 0u;" if 0 in self.vregs else "")
        out += body
        out.append("    return o;\n}")
        return "\n".join(x for x in out if x != "")

    def decls(self):
        decl = []
        sl = sorted(self.sregs)
        vl = sorted(self.vregs)
        if sl:
            decl.append("    uint " + ", ".join(f"s{n} = 0" for n in sl) + ";")
        if vl:
            decl.append("    uint " + ", ".join(f"v{n} = 0" for n in vl) + ";")
        decl.append("    uint m0 = 0, vccl = 0, vcch = 0; bool ex = true, vcc = false, scc = false;")
        if self.kind == "ps":
            decl.append("    bool done_ = false;  // the final export ran (else the pixel is killed)")
        if self.masks:
            decl.append("    bool " + ", ".join(f"k{n} = false" for n in sorted(self.masks)) + ";")
        if self.lane_spills:
            decl.append("    uint " + ", ".join(f"lane_v{v}_{l} = 0" for v, l in sorted(self.lane_spills)) + ";")
        if self.lds_slots:
            decl.append("    uint " + ", ".join(f"lds{o} = 0" for o in sorted(self.lds_slots)) + ";")
        return decl

    def pos_input_vgprs(self):
        """VGPRs that receive the pixel position x / y (input_addr bits 8 / 9)"""
        if self.kind != "ps":
            return {}
        addr = self.meta["regs"]["input_addr"]
        sizes = [2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1]
        v, out = 0, {}
        for bit, size in enumerate(sizes):
            if (addr >> bit) & 1:
                if bit in (8, 9):
                    out[v] = "xy"[bit - 8]
                v += size
        return out

    def floor_expr(self, i, a):
        """floor() of the pixel position (integer pixel coordinates, e.g. for view-ray
        reconstruction) keeps the sub-pixel position of supersampled targets: neighbouring pixels
        must differ, or derivatives of the result (normals from depth) degenerate. Equal to
        floor() at scale 1."""
        src = i.f.get("src0")
        if src is not None and getattr(src, "kind", None) == "v":
            axis = self.pos_input_vgprs().get(src.value)
            assigned = re.compile(rf"(?<![\w.])v{src.value} = ")
            if axis and not any(assigned.search(line) for line in self.lines) and \
                    self.feeds_float_math_only(i, i.f.get("vdst")):
                self.uses_scale = True
                return f"((floor(IN.pos.{axis}) + 0.5f) * ctr_scale.x - 0.5f)"
        return f"floor({a})"

    FLOAT_MATH = {"v_add_f32", "v_sub_f32", "v_subrev_f32", "v_mul_f32", "v_mul_legacy_f32", "v_mac_f32",
                  "v_mac_legacy_f32", "v_mad_f32", "v_mad_legacy_f32", "v_fma_f32", "v_madmk_f32", "v_madak_f32"}

    def feeds_float_math_only(self, i, d):
        """True if VGPR `d`, written by `i`, is only read by float arithmetic before it is
        overwritten (no integer conversion, memory address or other use)."""
        if d is None:
            return False
        k = self.insts.index(i)
        for j in self.insts[k + 1:k + 200]:
            reads = {o.value for key in ("src0", "src1", "src2") if (o := j.f.get(key)) is not None
                     and getattr(o, "kind", None) == "v"}
            if "vsrc1" in j.f:
                reads.add(j.f["vsrc1"])
            if j.fmt in ("MIMG", "MUBUF", "MTBUF", "DS", "EXP", "VINTRP", "FLAT"):
                ignore = {"op", "offset", "offset0", "offset1", "dmask", "en", "gds", "vdst"}
                if j.name.startswith(("ds_read", "ds_swizzle")):
                    ignore |= {"data0", "data1"}  # not read by these
                for key, val in j.f.items():
                    if isinstance(val, int) and key not in ignore and val <= d < val + 4:
                        return False
            if d in reads and j.name not in self.FLOAT_MATH:
                return False
            if j.f.get("vdst") == d:  # overwritten (VOP*, DPP, SDWA)
                return True
            if j.name in ("s_endpgm",):
                return True
        return True

    def ps_input_vgprs(self):
        addr = self.meta["regs"]["input_addr"]
        sizes = [2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1]
        v = 0
        lines = []
        for bit, size in enumerate(sizes):
            if not (addr >> bit) & 1:
                continue
            if bit == 8 and v in self.vregs:
                self.uses_scale = True
                lines.append(f"    v{v} = asuint(IN.pos.x * ctr_scale.x);")
            if bit == 9 and v in self.vregs:
                self.uses_scale = True
                lines.append(f"    v{v} = asuint(IN.pos.y * ctr_scale.x);")
            if bit == 10 and v in self.vregs:
                lines.append(f"    v{v} = asuint(IN.pos.z);")
            if bit == 11 and v in self.vregs:
                lines.append(f"    v{v} = asuint(IN.pos.w);")
            if bit == 12 and v in self.vregs:
                lines.append(f"    v{v} = asuint(IN.ff ? 1.0f : -1.0f);")  # +1.0/-1.0 like the hardware
            v += size
        return lines


VSOUT_STRUCT = "struct VSOut {\n    float4 pos : SV_Position;\n" + "".join(
    f"    float4 a{s} : VARYING{s};\n" for s in VARYINGS) + "};"
ESOUT_STRUCT = f"struct ESOut {{\n    float4 e[{ES_VECS}] : ESGS0;\n}};"

COMMON_HLSL = r"""
float cubeid(float x, float y, float z) {
    float ax = abs(x), ay = abs(y), az = abs(z);
    if (az >= ax && az >= ay) return z < 0 ? 5.0 : 4.0;
    if (ay >= ax) return y < 0 ? 3.0 : 2.0;
    return x < 0 ? 1.0 : 0.0;
}
float cubesc(float x, float y, float z) {
    float ax = abs(x), ay = abs(y), az = abs(z);
    if (az >= ax && az >= ay) return z < 0 ? -x : x;
    if (ay >= ax) return x;
    return x < 0 ? z : -z;
}
float cubetc(float x, float y, float z) {
    float ax = abs(x), ay = abs(y), az = abs(z);
    if (az >= ax && az >= ay) return -y;
    if (ay >= ax) return y < 0 ? -z : z;
    return -y;
}
float cubema(float x, float y, float z) {
    float ax = abs(x), ay = abs(y), az = abs(z);
    if (az >= ax && az >= ay) return 2.0 * z;
    if (ay >= ax) return 2.0 * y;
    return 2.0 * x;
}
float3 cubedir(float u, float v, float f) {
    float s = 2.0 * u - 1.0, t = 2.0 * v - 1.0;
    uint face = uint(f + 0.5);
    if (face == 0u) return float3(1.0, -t, -s);
    if (face == 1u) return float3(-1.0, -t, s);
    if (face == 2u) return float3(s, 1.0, t);
    if (face == 3u) return float3(s, -1.0, -t);
    if (face == 4u) return float3(s, -t, 1.0);
    return float3(-s, -t, -1.0);
}
bool cmp_class(float v, uint m) {
    uint u = asuint(v);
    bool neg = (u >> 31) != 0;
    uint e = (u >> 23) & 0xFFu, f = u & 0x7FFFFFu;
    bool r = false;
    if (e == 0xFFu && f != 0) r = r || ((m & 3u) != 0);
    if (e == 0xFFu && f == 0) r = r || ((m & (neg ? 4u : 512u)) != 0);
    if (e != 0 && e != 0xFFu) r = r || ((m & (neg ? 8u : 256u)) != 0);
    if (e == 0 && f != 0) r = r || ((m & (neg ? 16u : 128u)) != 0);
    if (e == 0 && f == 0) r = r || ((m & (neg ? 32u : 64u)) != 0);
    return r;
}
float quad_swizzle(float v, uint perm, uint lane) {
    float dx = ddx_fine(v), dy = ddy_fine(v);
    uint src = (perm >> (lane * 2u)) & 3u;
    float r = v;
    if ((src ^ lane) & 1u) r += (lane & 1u) ? -dx : dx;
    if ((src ^ lane) & 2u) r += (lane & 2u) ? -dy : dy;
    return r;
}
bool quad_any(bool c) {
    float v = c ? 1.0 : 0.0;
    float r = (v != 0.0 || ddx_fine(v) != 0.0) ? 1.0 : 0.0;
    return r != 0.0 || ddy_fine(r) != 0.0;
}
"""

# ------------------------------------------------------------------ D3DCompile
_d3dc = None


def d3d_compile(src, target):
    global _d3dc
    if _d3dc is None:
        _d3dc = ctypes.WinDLL("d3dcompiler_47.dll")
        _d3dc.D3DCompile.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p,
                                     ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint,
                                     ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p),
                                     ctypes.POINTER(ctypes.c_void_p)]
    code, err = ctypes.c_void_p(), ctypes.c_void_p()
    b = src.encode()
    hr = _d3dc.D3DCompile(b, len(b), None, None, None, b"main", target.encode(),
                          1 << 14 | 1 << 11, 0, ctypes.byref(code), ctypes.byref(err))

    def blob(p):
        if not p.value:
            return b""
        vt = ctypes.cast(p, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
        get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
        release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])
        data = ctypes.string_at(get_ptr(p), get_size(p))
        release(p)
        return data

    return hr & 0xFFFFFFFF, blob(code), blob(err).decode(errors="replace")


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def translate_gs(code, meta):
    """GS binary = GS program + footer, then the copy VS at the next 256-byte boundary"""
    import math
    p = code.find(b"OrbShdr")
    glen = struct.unpack_from("<I", code, p + 8)[0] >> 8
    cstart = (p // 256 + 1) * 256
    cinfo = gcn.parse_footer(code)
    cinsts = gcn.decode(code, cstart, cstart + cinfo["length"])
    for i in cinsts:
        i.pc -= cstart
    offs = [i.f["offset"] for i in cinsts if i.name == "buffer_load_dword" and i.f["offset"]]
    if not offs:
        raise Unsupported("copy shader ring stride")
    g = 0
    for o in offs:
        g = math.gcd(g, o)
    max_vert = g // 64
    copy = Translator("vs", cinsts, meta["gnm"]["copy"], cinfo["length"],
                      ring={"role": "copy", "max_vert": max_vert})
    copy_fn = copy.function_text(f"VSOut copyvs(uint gso[{GSO_LEN}])")
    ring = {"role": "gs", "max_vert": max_vert, "copy_fn": copy_fn,
            "out_verts": min(max_vert, 1024 // GS_OUT_SCALARS)}
    t = Translator("gs", gcn.decode(code, 0, glen), meta["gnm"], glen, ring=ring)
    return t.translate(), t


def translate_one(path, meta):
    code = open(path, "rb").read()
    if meta["kind"] == "gs":
        return translate_gs(code, meta)
    info = gcn.parse_footer(code)
    insts = gcn.decode(code, 0, info["length"])
    ring = {"role": "es"} if meta["kind"] == "es" else None
    t = Translator(meta["kind"], insts, meta["gnm"], info["length"], ring=ring)
    hlsl = t.translate()
    mixed = {slot: 3 for slot, d in t.dims_seen.items() if len(d) > 1}
    if mixed:  # a slot sampled both as 2D and as cube/array: declare it as an array
        t = Translator(meta["kind"], insts, meta["gnm"], info["length"], mixed, ring=ring)
        hlsl = t.translate()
    cubes = t.cube_used - t.cube_bad
    if cubes:  # slots only sampled as cubes: TextureCube
        t = Translator(meta["kind"], insts, meta["gnm"], info["length"], mixed or None, ring=ring,
                       cube_slots=cubes)
        hlsl = t.translate()
    return hlsl, t


def runtime_index_lines(e, t, code):
    """records read by port/src/gfx/shaders.cpp"""
    # the runtime hashes the program the Gnm struct points at (a GS without its copy shader)
    out = [f"S {fnv1a64(code[:e['gnm']['shader_size']]):016x} {e['name']} {e['kind']}"]
    for slot, (dims, _use) in sorted(t.textures.items()):
        out.append(f"T {slot} {dims}")
    for slot, kind in sorted(t.samplers.items()):
        out.append(f"M {slot} {kind}")
    for slot in sorted(t.cbuffers):
        out.append(f"B {slot}")
    if e["kind"] in ("vs", "es"):
        for inp in e["gnm"].get("inputs", []):
            out.append(f"I {inp['sem']} {inp['size']}")
    return out


def work(args):
    """translate + compile one shader (runs in a worker process)"""
    src_dir, out_dir, e = args
    try:
        hlsl, tr = translate_one(os.path.join(src_dir, e["name"] + ".bin"), e)
    except (Unsupported, gcn.DecodeError) as ex:
        return e["name"], "unsupported", str(ex), []
    open(os.path.join(out_dir, e["name"] + ".hlsl"), "w").write(hlsl)
    if os.environ.get("GCN2HLSL_NO_FXC") == "1":
        # Vulkan lab: DXC compiles the HLSL afterwards (build_spirv.py); fxc's D3D11 check (the
        # slowest step by far) is not needed there
        code = open(os.path.join(src_dir, e["name"] + ".bin"), "rb").read()
        return e["name"], "compiled", "", runtime_index_lines(e, tr, code)
    hr, blob, err = d3d_compile(hlsl, {"vs": "vs_5_0", "es": "vs_5_0", "gs": "gs_5_0"}.get(e["kind"], "ps_5_0"))
    if hr != 0:
        first = next((l for l in err.splitlines() if "error" in l), err[:200])
        return e["name"], "compile_error", first, []
    open(os.path.join(out_dir, e["name"] + ".dxbc"), "wb").write(blob)
    code = open(os.path.join(src_dir, e["name"] + ".bin"), "rb").read()
    return e["name"], "compiled", "", runtime_index_lines(e, tr, code)


def main(src_dir, out_dir, filt=None):
    import multiprocessing as mp
    os.makedirs(out_dir, exist_ok=True)
    index = json.load(open(os.path.join(src_dir, "index.json")))
    done, stats, reasons, comp_err = set(), Counter(), Counter(), Counter()
    results = {}
    rt_index = []
    jobs = []
    for e in index:
        if e["name"] in done or e["kind"] not in ("vs", "ps", "es", "gs") or (filt and filt not in e["name"]):
            continue
        done.add(e["name"])
        jobs.append((src_dir, out_dir, e))
    with mp.Pool() as pool:
        for name, status, reason, lines in pool.imap_unordered(work, jobs, chunksize=8):
            stats[status] += 1
            n = sum(stats.values())
            if n % 50 == 0 or n == len(jobs):
                print(f"translated {n}/{len(jobs)}", flush=True)
            if status == "compiled":
                results[name] = {"ok": True}
                rt_index += lines
            elif status == "unsupported":
                reasons[reason[:60]] += 1
                results[name] = {"ok": False, "reason": reason}
            else:
                comp_err[reason.split("): ")[-1][:80]] += 1
                results[name] = {"ok": False, "reason": "fxc: " + reason}
    json.dump(results, open(os.path.join(out_dir, "results.json"), "w"), indent=1)
    if not filt:
        with open(os.path.join(out_dir, "shaders.idx"), "w") as f:
            f.write("\n".join(rt_index) + "\n")
    total = sum(stats.values())
    print(f"{total} shaders: {dict(stats)}")
    print("unsupported:", reasons.most_common(15))
    print("compile errors:", comp_err.most_common(15))


if __name__ == "__main__":
    main(*sys.argv[1:4])
