"""GCN2 (Sea Islands / PS4 "Liverpool") shader decoder + PS4 shader binary footer parser.

usage: python gcn.py <shader.bin>      -> disassembly + footer info
"""
import struct
import sys
from dataclasses import dataclass, field

# ------------------------------------------------------------------ opcode tables
SOP2 = {0: "s_add_u32", 1: "s_sub_u32", 2: "s_add_i32", 3: "s_sub_i32", 4: "s_addc_u32",
        5: "s_subb_u32", 6: "s_min_i32", 7: "s_min_u32", 8: "s_max_i32", 9: "s_max_u32",
        10: "s_cselect_b32", 11: "s_cselect_b64", 14: "s_and_b32", 15: "s_and_b64",
        16: "s_or_b32", 17: "s_or_b64", 18: "s_xor_b32", 19: "s_xor_b64", 20: "s_andn2_b32",
        21: "s_andn2_b64", 22: "s_orn2_b32", 23: "s_orn2_b64", 24: "s_nand_b32",
        25: "s_nand_b64", 26: "s_nor_b32", 27: "s_nor_b64", 28: "s_xnor_b32", 29: "s_xnor_b64",
        30: "s_lshl_b32", 31: "s_lshl_b64", 32: "s_lshr_b32", 33: "s_lshr_b64",
        34: "s_ashr_i32", 35: "s_ashr_i64", 36: "s_bfm_b32", 37: "s_bfm_b64", 38: "s_mul_i32",
        39: "s_bfe_u32", 40: "s_bfe_i32", 41: "s_bfe_u64", 42: "s_bfe_i64",
        43: "s_cbranch_g_fork", 44: "s_absdiff_i32"}
SOPK = {0: "s_movk_i32", 2: "s_cmovk_i32", 3: "s_cmpk_eq_i32", 4: "s_cmpk_lg_i32",
        5: "s_cmpk_gt_i32", 6: "s_cmpk_ge_i32", 7: "s_cmpk_lt_i32", 8: "s_cmpk_le_i32",
        9: "s_cmpk_eq_u32", 10: "s_cmpk_lg_u32", 11: "s_cmpk_gt_u32", 12: "s_cmpk_ge_u32",
        13: "s_cmpk_lt_u32", 14: "s_cmpk_le_u32", 15: "s_addk_i32", 16: "s_mulk_i32",
        17: "s_cbranch_i_fork", 18: "s_getreg_b32", 19: "s_setreg_b32",
        21: "s_setreg_imm32_b32"}
SOP1 = {3: "s_mov_b32", 4: "s_mov_b64", 5: "s_cmov_b32", 6: "s_cmov_b64", 7: "s_not_b32",
        8: "s_not_b64", 9: "s_wqm_b32", 10: "s_wqm_b64", 11: "s_brev_b32", 12: "s_brev_b64",
        13: "s_bcnt0_i32_b32", 14: "s_bcnt0_i32_b64", 15: "s_bcnt1_i32_b32",
        16: "s_bcnt1_i32_b64", 17: "s_ff0_i32_b32", 18: "s_ff0_i32_b64", 19: "s_ff1_i32_b32",
        20: "s_ff1_i32_b64", 21: "s_flbit_i32_b32", 22: "s_flbit_i32_b64", 23: "s_flbit_i32",
        24: "s_flbit_i32_i64", 25: "s_sext_i32_i8", 26: "s_sext_i32_i16", 27: "s_bitset0_b32",
        28: "s_bitset0_b64", 29: "s_bitset1_b32", 30: "s_bitset1_b64", 31: "s_getpc_b64",
        32: "s_setpc_b64", 33: "s_swappc_b64", 34: "s_rfe_b64", 36: "s_and_saveexec_b64",
        37: "s_or_saveexec_b64", 38: "s_xor_saveexec_b64", 39: "s_andn2_saveexec_b64",
        40: "s_orn2_saveexec_b64", 41: "s_nand_saveexec_b64", 42: "s_nor_saveexec_b64",
        43: "s_xnor_saveexec_b64", 44: "s_quadmask_b32", 45: "s_quadmask_b64",
        46: "s_movrels_b32", 47: "s_movrels_b64", 48: "s_movreld_b32", 49: "s_movreld_b64",
        50: "s_cbranch_join", 52: "s_abs_i32", 53: "s_mov_fed_b32"}
SOPC = {0: "s_cmp_eq_i32", 1: "s_cmp_lg_i32", 2: "s_cmp_gt_i32", 3: "s_cmp_ge_i32",
        4: "s_cmp_lt_i32", 5: "s_cmp_le_i32", 6: "s_cmp_eq_u32", 7: "s_cmp_lg_u32",
        8: "s_cmp_gt_u32", 9: "s_cmp_ge_u32", 10: "s_cmp_lt_u32", 11: "s_cmp_le_u32",
        12: "s_bitcmp0_b32", 13: "s_bitcmp1_b32", 14: "s_bitcmp0_b64", 15: "s_bitcmp1_b64",
        16: "s_setvskip"}
SOPP = {0: "s_nop", 1: "s_endpgm", 2: "s_branch", 4: "s_cbranch_scc0", 5: "s_cbranch_scc1",
        6: "s_cbranch_vccz", 7: "s_cbranch_vccnz", 8: "s_cbranch_execz", 9: "s_cbranch_execnz",
        10: "s_barrier", 12: "s_waitcnt", 13: "s_sethalt", 14: "s_sleep", 15: "s_setprio",
        16: "s_sendmsg", 17: "s_sendmsghalt", 18: "s_trap", 19: "s_icache_inv",
        20: "s_incperflevel", 21: "s_decperflevel", 22: "s_ttracedata"}
SMRD = {0: "s_load_dword", 1: "s_load_dwordx2", 2: "s_load_dwordx4", 3: "s_load_dwordx8",
        4: "s_load_dwordx16", 8: "s_buffer_load_dword", 9: "s_buffer_load_dwordx2",
        10: "s_buffer_load_dwordx4", 11: "s_buffer_load_dwordx8", 12: "s_buffer_load_dwordx16",
        29: "s_dcache_inv_vol", 30: "s_memtime", 31: "s_dcache_inv"}
VOP2 = {0: "v_cndmask_b32", 1: "v_readlane_b32", 2: "v_writelane_b32", 3: "v_add_f32",
        4: "v_sub_f32", 5: "v_subrev_f32", 6: "v_mac_legacy_f32", 7: "v_mul_legacy_f32",
        8: "v_mul_f32", 9: "v_mul_i32_i24", 10: "v_mul_hi_i32_i24", 11: "v_mul_u32_u24",
        12: "v_mul_hi_u32_u24", 13: "v_min_legacy_f32", 14: "v_max_legacy_f32",
        15: "v_min_f32", 16: "v_max_f32", 17: "v_min_i32", 18: "v_max_i32", 19: "v_min_u32",
        20: "v_max_u32", 21: "v_lshr_b32", 22: "v_lshrrev_b32", 23: "v_ashr_i32",
        24: "v_ashrrev_i32", 25: "v_lshl_b32", 26: "v_lshlrev_b32", 27: "v_and_b32",
        28: "v_or_b32", 29: "v_xor_b32", 30: "v_bfm_b32", 31: "v_mac_f32", 32: "v_madmk_f32",
        33: "v_madak_f32", 34: "v_bcnt_u32_b32", 35: "v_mbcnt_lo_u32_b32",
        36: "v_mbcnt_hi_u32_b32", 37: "v_add_i32", 38: "v_sub_i32", 39: "v_subrev_i32",
        40: "v_addc_u32", 41: "v_subb_u32", 42: "v_subbrev_u32", 43: "v_ldexp_f32",
        44: "v_cvt_pkaccum_u8_f32", 45: "v_cvt_pknorm_i16_f32", 46: "v_cvt_pknorm_u16_f32",
        47: "v_cvt_pkrtz_f16_f32", 48: "v_cvt_pk_u16_u32", 49: "v_cvt_pk_i16_i32"}
VOP1 = {0: "v_nop", 1: "v_mov_b32", 2: "v_readfirstlane_b32", 3: "v_cvt_i32_f64",
        4: "v_cvt_f64_i32", 5: "v_cvt_f32_i32", 6: "v_cvt_f32_u32", 7: "v_cvt_u32_f32",
        8: "v_cvt_i32_f32", 9: "v_mov_fed_b32", 10: "v_cvt_f16_f32", 11: "v_cvt_f32_f16",
        12: "v_cvt_rpi_i32_f32", 13: "v_cvt_flr_i32_f32", 14: "v_cvt_off_f32_i4",
        15: "v_cvt_f32_f64", 16: "v_cvt_f64_f32", 17: "v_cvt_f32_ubyte0",
        18: "v_cvt_f32_ubyte1", 19: "v_cvt_f32_ubyte2", 20: "v_cvt_f32_ubyte3",
        21: "v_cvt_u32_f64", 22: "v_cvt_f64_u32", 23: "v_trunc_f64", 24: "v_ceil_f64",
        25: "v_rndne_f64", 26: "v_floor_f64", 32: "v_fract_f32", 33: "v_trunc_f32",
        34: "v_ceil_f32", 35: "v_rndne_f32", 36: "v_floor_f32", 37: "v_exp_f32",
        38: "v_log_clamp_f32", 39: "v_log_f32", 40: "v_rcp_clamp_f32", 41: "v_rcp_legacy_f32",
        42: "v_rcp_f32", 43: "v_rcp_iflag_f32", 44: "v_rsq_clamp_f32", 45: "v_rsq_legacy_f32",
        46: "v_rsq_f32", 47: "v_rcp_f64", 48: "v_rcp_clamp_f64", 49: "v_rsq_f64",
        50: "v_rsq_clamp_f64", 51: "v_sqrt_f32", 52: "v_sqrt_f64", 53: "v_sin_f32",
        54: "v_cos_f32", 55: "v_not_b32", 56: "v_bfrev_b32", 57: "v_ffbh_u32", 58: "v_ffbl_b32",
        59: "v_ffbh_i32", 60: "v_frexp_exp_i32_f64", 61: "v_frexp_mant_f64",
        62: "v_fract_f64", 63: "v_frexp_exp_i32_f32", 64: "v_frexp_mant_f32",
        65: "v_clrexcp", 66: "v_movreld_b32", 67: "v_movrels_b32", 68: "v_movrelsd_b32",
        69: "v_log_legacy_f32", 70: "v_exp_legacy_f32"}
_FCMP = ["f", "lt", "eq", "le", "gt", "lg", "ge", "o", "u", "nge", "nlg", "ngt", "nle", "neq",
         "nlt", "tru"]
_ICMP = ["f", "lt", "eq", "le", "gt", "ne", "ge", "t"]
VOPC = {}
for i, c in enumerate(_FCMP):
    VOPC[0x00 + i] = f"v_cmp_{c}_f32"
    VOPC[0x10 + i] = f"v_cmpx_{c}_f32"
    VOPC[0x20 + i] = f"v_cmp_{c}_f64"
    VOPC[0x30 + i] = f"v_cmpx_{c}_f64"
    VOPC[0x40 + i] = f"v_cmps_{c}_f32"
    VOPC[0x50 + i] = f"v_cmpsx_{c}_f32"
    VOPC[0x60 + i] = f"v_cmps_{c}_f64"
    VOPC[0x70 + i] = f"v_cmpsx_{c}_f64"
for i, c in enumerate(_ICMP):
    VOPC[0x80 + i] = f"v_cmp_{c}_i32"
    VOPC[0x90 + i] = f"v_cmpx_{c}_i32"
    VOPC[0xA0 + i] = f"v_cmp_{c}_i64"
    VOPC[0xB0 + i] = f"v_cmpx_{c}_i64"
    VOPC[0xC0 + i] = f"v_cmp_{c}_u32"
    VOPC[0xD0 + i] = f"v_cmpx_{c}_u32"
    VOPC[0xE0 + i] = f"v_cmp_{c}_u64"
    VOPC[0xF0 + i] = f"v_cmpx_{c}_u64"
VOPC.update({0x88: "v_cmp_class_f32", 0x98: "v_cmpx_class_f32", 0xA8: "v_cmp_class_f64",
             0xB8: "v_cmpx_class_f64"})
VOP3_ONLY = {320: "v_mad_legacy_f32", 321: "v_mad_f32", 322: "v_mad_i32_i24",
             323: "v_mad_u32_u24", 324: "v_cubeid_f32", 325: "v_cubesc_f32",
             326: "v_cubetc_f32", 327: "v_cubema_f32", 328: "v_bfe_u32", 329: "v_bfe_i32",
             330: "v_bfi_b32", 331: "v_fma_f32", 332: "v_fma_f64", 333: "v_lerp_u8",
             334: "v_alignbit_b32", 335: "v_alignbyte_b32", 336: "v_mullit_f32",
             337: "v_min3_f32", 338: "v_min3_i32", 339: "v_min3_u32", 340: "v_max3_f32",
             341: "v_max3_i32", 342: "v_max3_u32", 343: "v_med3_f32", 344: "v_med3_i32",
             345: "v_med3_u32", 346: "v_sad_u8", 347: "v_sad_hi_u8", 348: "v_sad_u16",
             349: "v_sad_u32", 350: "v_cvt_pk_u8_f32", 351: "v_div_fixup_f32",
             352: "v_div_fixup_f64", 353: "v_lshl_b64", 354: "v_lshr_b64", 355: "v_ashr_i64",
             356: "v_add_f64", 357: "v_mul_f64", 358: "v_min_f64", 359: "v_max_f64",
             360: "v_ldexp_f64", 361: "v_mul_lo_u32", 362: "v_mul_hi_u32", 363: "v_mul_lo_i32",
             364: "v_mul_hi_i32", 365: "v_div_scale_f32", 366: "v_div_scale_f64",
             367: "v_div_fmas_f32", 368: "v_div_fmas_f64", 369: "v_msad_u8",
             370: "v_qsad_pk_u16_u8", 371: "v_mqsad_pk_u16_u8", 372: "v_trig_preop_f64",
             373: "v_mqsad_u32_u8", 374: "v_mad_u64_u32", 375: "v_mad_i64_i32"}
VINTRP = {0: "v_interp_p1_f32", 1: "v_interp_p2_f32", 2: "v_interp_mov_f32"}
MUBUF = {0: "buffer_load_format_x", 1: "buffer_load_format_xy", 2: "buffer_load_format_xyz",
         3: "buffer_load_format_xyzw", 4: "buffer_store_format_x", 5: "buffer_store_format_xy",
         6: "buffer_store_format_xyz", 7: "buffer_store_format_xyzw", 8: "buffer_load_ubyte",
         9: "buffer_load_sbyte", 10: "buffer_load_ushort", 11: "buffer_load_sshort",
         12: "buffer_load_dword", 13: "buffer_load_dwordx2", 14: "buffer_load_dwordx4",
         15: "buffer_load_dwordx3", 24: "buffer_store_byte", 26: "buffer_store_short",
         28: "buffer_store_dword", 29: "buffer_store_dwordx2", 30: "buffer_store_dwordx4",
         31: "buffer_store_dwordx3", 50: "buffer_atomic_add", 112: "buffer_wbinvl1_vol",
         113: "buffer_wbinvl1"}
MTBUF = {0: "tbuffer_load_format_x", 1: "tbuffer_load_format_xy", 2: "tbuffer_load_format_xyz",
         3: "tbuffer_load_format_xyzw", 4: "tbuffer_store_format_x",
         5: "tbuffer_store_format_xy", 6: "tbuffer_store_format_xyz",
         7: "tbuffer_store_format_xyzw"}
_SAMPLE = ["", "_cl", "_d", "_d_cl", "_l", "_b", "_b_cl", "_lz", "_c", "_c_cl", "_c_d",
           "_c_d_cl", "_c_l", "_c_b", "_c_b_cl", "_c_lz"]
_GATHER = ["", "_cl", "_l", "_b", "_b_cl", "_lz", "_c", "_c_cl", "_c_l", "_c_b", "_c_b_cl",
           "_c_lz"]
MIMG = {0: "image_load", 1: "image_load_mip", 2: "image_load_pck", 3: "image_load_pck_sgn",
        4: "image_load_mip_pck", 5: "image_load_mip_pck_sgn", 8: "image_store",
        9: "image_store_mip", 10: "image_store_pck", 11: "image_store_mip_pck",
        14: "image_get_resinfo", 96: "image_get_lod"}
for i, s in enumerate(_SAMPLE):
    MIMG[32 + i] = "image_sample" + s
    MIMG[48 + i] = "image_sample" + s + "_o"
for i, s in enumerate(_SAMPLE):
    if "_d" not in s:  # gather4 mirrors the sample layout, without derivative variants
        MIMG[64 + i] = "image_gather4" + s
        MIMG[80 + i] = "image_gather4" + s + "_o"
for i, s in enumerate(["_cd", "_cd_cl", "_c_cd", "_c_cd_cl", "_cd_o", "_cd_cl_o", "_c_cd_o",
                       "_c_cd_cl_o"]):
    MIMG[104 + i] = "image_sample" + s
DS = {0: "ds_add_u32", 13: "ds_write_b32", 14: "ds_write2_b32", 53: "ds_swizzle_b32",
      54: "ds_read_b32", 55: "ds_read2_b32", 78: "ds_write_b64", 118: "ds_read_b64"}

SPECIAL = {106: "vcc_lo", 107: "vcc_hi", 124: "m0", 126: "exec_lo", 127: "exec_hi",
           251: "vccz", 252: "execz", 253: "scc", 254: "lds_direct"}
INLINE_F = {240: 0.5, 241: -0.5, 242: 1.0, 243: -1.0, 244: 2.0, 245: -2.0, 246: 4.0,
            247: -4.0}


@dataclass
class Operand:
    kind: str  # "s", "v", "lit", "int", "float", "special"
    value: object

    def __str__(self):
        if self.kind == "s":
            return f"s{self.value}"
        if self.kind == "v":
            return f"v{self.value}"
        if self.kind == "lit":
            return f"0x{self.value:08x}"
        if self.kind in ("int", "float"):
            return str(self.value)
        return str(self.value)


@dataclass
class Inst:
    pc: int
    fmt: str
    op: int
    name: str
    size: int
    f: dict = field(default_factory=dict)  # decoded fields

    def __str__(self):
        args = ", ".join(f"{k}={v}" for k, v in self.f.items())
        return f"{self.pc:04x}: {self.name:28} {args}"


def src_operand(v, literal):
    if v <= 103:
        return Operand("s", v)
    if v in SPECIAL:
        return Operand("special", SPECIAL[v])
    if 128 <= v <= 192:
        return Operand("int", v - 128)
    if 193 <= v <= 208:
        return Operand("int", 192 - v)
    if v in INLINE_F:
        return Operand("float", INLINE_F[v])
    if v == 255:
        return Operand("lit", literal)
    if 256 <= v <= 511:
        return Operand("v", v - 256)
    return Operand("special", f"src{v}")


class DecodeError(Exception):
    pass


def decode(code: bytes, start=0, end=None):
    end = len(code) if end is None else end
    pc = start
    out = []
    while pc < end:
        w0, = struct.unpack_from("<I", code, pc)
        w1 = struct.unpack_from("<I", code, pc + 4)[0] if pc + 8 <= len(code) else 0
        inst = decode_one(pc, w0, w1)
        out.append(inst)
        pc += inst.size
    return out


def decode_one(pc, w0, w1):
    top2 = w0 >> 30
    if top2 == 2:  # scalar ALU family
        top9 = w0 >> 23
        if top9 == 0x17D:
            op = (w0 >> 8) & 0xFF
            s0 = w0 & 0xFF
            i = Inst(pc, "SOP1", op, SOP1.get(op, f"sop1_{op}"), 8 if s0 == 255 else 4)
            i.f = {"sdst": (w0 >> 16) & 0x7F, "ssrc0": src_operand(s0, w1)}
            return i
        if top9 == 0x17E:
            op = (w0 >> 16) & 0x7F
            s0, s1 = w0 & 0xFF, (w0 >> 8) & 0xFF
            i = Inst(pc, "SOPC", op, SOPC.get(op, f"sopc_{op}"),
                     8 if 255 in (s0, s1) else 4)
            i.f = {"ssrc0": src_operand(s0, w1), "ssrc1": src_operand(s1, w1)}
            return i
        if top9 == 0x17F:
            op = (w0 >> 16) & 0x7F
            i = Inst(pc, "SOPP", op, SOPP.get(op, f"sopp_{op}"), 4)
            i.f = {"simm16": w0 & 0xFFFF}
            return i
        if (w0 >> 28) == 0xB:
            op = (w0 >> 23) & 0x1F
            i = Inst(pc, "SOPK", op, SOPK.get(op, f"sopk_{op}"), 8 if op == 21 else 4)
            i.f = {"sdst": (w0 >> 16) & 0x7F, "simm16": w0 & 0xFFFF}
            if op == 21:
                i.f["lit"] = w1
            return i
        op = (w0 >> 23) & 0x7F
        s0, s1 = w0 & 0xFF, (w0 >> 8) & 0xFF
        i = Inst(pc, "SOP2", op, SOP2.get(op, f"sop2_{op}"), 8 if 255 in (s0, s1) else 4)
        i.f = {"sdst": (w0 >> 16) & 0x7F, "ssrc0": src_operand(s0, w1),
               "ssrc1": src_operand(s1, w1)}
        return i
    if (w0 >> 31) == 0:  # VOP2 / VOP1 / VOPC
        top7 = w0 >> 25
        s0 = w0 & 0x1FF
        size = 8 if s0 == 255 else 4
        if top7 == 0x3F:
            op = (w0 >> 9) & 0xFF
            i = Inst(pc, "VOP1", op, VOP1.get(op, f"vop1_{op}"), size)
            i.f = {"vdst": (w0 >> 17) & 0xFF, "src0": src_operand(s0, w1)}
            return i
        if top7 == 0x3E:
            op = (w0 >> 17) & 0xFF
            i = Inst(pc, "VOPC", op, VOPC.get(op, f"vopc_{op}"), size)
            i.f = {"src0": src_operand(s0, w1), "vsrc1": (w0 >> 9) & 0xFF}
            return i
        op = (w0 >> 25) & 0x3F
        name = VOP2.get(op, f"vop2_{op}")
        if op in (32, 33):  # madmk/madak carry a literal
            size = 8
        i = Inst(pc, "VOP2", op, name, size)
        i.f = {"vdst": (w0 >> 17) & 0xFF, "src0": src_operand(s0, w1),
               "vsrc1": (w0 >> 9) & 0xFF}
        if op in (32, 33):
            i.f["lit"] = w1
        return i
    top6 = w0 >> 26
    if (w0 >> 27) == 0x18:  # SMRD
        op = (w0 >> 22) & 0x1F
        imm = (w0 >> 8) & 1
        off = w0 & 0xFF
        size = 8 if (not imm and off == 255) else 4
        i = Inst(pc, "SMRD", op, SMRD.get(op, f"smrd_{op}"), size)
        i.f = {"sdst": (w0 >> 15) & 0x7F, "sbase": ((w0 >> 9) & 0x3F) * 2, "imm": imm,
               "offset": off}
        if size == 8:
            i.f["lit"] = w1
        return i
    if top6 == 0x34:  # VOP3
        op = (w0 >> 17) & 0x1FF
        if op < 256:
            name, base = VOPC.get(op, f"vopc_{op}"), "VOPC"
        elif op < 320:
            name, base = VOP2.get(op - 256, f"vop2_{op - 256}"), "VOP2"
        elif op < 384:
            name, base = VOP3_ONLY.get(op, f"vop3_{op}"), "VOP3"
        else:
            name, base = VOP1.get(op - 384, f"vop1_{op - 384}"), "VOP1"
        i = Inst(pc, "VOP3", op, name, 8)
        sdst_form = base == "VOPC" or name in ("v_add_i32", "v_sub_i32", "v_subrev_i32",
                                                "v_addc_u32", "v_subb_u32", "v_subbrev_u32",
                                                "v_div_scale_f32", "v_div_scale_f64")
        i.f = {"base": base, "vdst": w0 & 0xFF,
               "src0": src_operand(w1 & 0x1FF, None), "src1": src_operand((w1 >> 9) & 0x1FF, None),
               "src2": src_operand((w1 >> 18) & 0x1FF, None),
               "omod": (w1 >> 27) & 3, "neg": (w1 >> 29) & 7}
        if base == "VOPC":
            i.f["sdst"] = w0 & 0xFF
        elif sdst_form:
            i.f["sdst"] = (w0 >> 8) & 0x7F
        else:
            i.f["abs"] = (w0 >> 8) & 7
            i.f["clamp"] = (w0 >> 11) & 1
        return i
    if top6 == 0x32:  # VINTRP
        op = (w0 >> 16) & 3
        i = Inst(pc, "VINTRP", op, VINTRP[op] if op in VINTRP else f"vintrp_{op}", 4)
        i.f = {"vdst": (w0 >> 18) & 0xFF, "attr": (w0 >> 10) & 0x3F, "chan": (w0 >> 8) & 3,
               "vsrc": w0 & 0xFF}
        return i
    if top6 == 0x36:  # DS
        op = (w0 >> 18) & 0xFF
        i = Inst(pc, "DS", op, DS.get(op, f"ds_{op}"), 8)
        i.f = {"offset0": w0 & 0xFF, "offset1": (w0 >> 8) & 0xFF, "gds": (w0 >> 17) & 1,
               "addr": w1 & 0xFF, "data0": (w1 >> 8) & 0xFF, "data1": (w1 >> 16) & 0xFF,
               "vdst": w1 >> 24}
        return i
    if top6 in (0x38, 0x3A):  # MUBUF / MTBUF
        mt = top6 == 0x3A
        op = (w0 >> 16) & 7 if mt else (w0 >> 18) & 0x7F
        table = MTBUF if mt else MUBUF
        i = Inst(pc, "MTBUF" if mt else "MUBUF", op, table.get(op, f"buf_{op}"), 8)
        i.f = {"offset": w0 & 0xFFF, "offen": (w0 >> 12) & 1, "idxen": (w0 >> 13) & 1,
               "glc": (w0 >> 14) & 1, "addr64": (w0 >> 15) & 1, "vaddr": w1 & 0xFF,
               "vdata": (w1 >> 8) & 0xFF, "srsrc": ((w1 >> 16) & 0x1F) * 4,
               "soffset": src_operand(w1 >> 24, None)}
        if mt:
            i.f["dfmt"] = (w0 >> 19) & 0xF
            i.f["nfmt"] = (w0 >> 23) & 7
        return i
    if top6 == 0x3C:  # MIMG
        op = (w0 >> 18) & 0x7F
        i = Inst(pc, "MIMG", op, MIMG.get(op, f"mimg_{op}"), 8)
        i.f = {"dmask": (w0 >> 8) & 0xF, "unorm": (w0 >> 12) & 1, "da": (w0 >> 14) & 1,
               "vaddr": w1 & 0xFF, "vdata": (w1 >> 8) & 0xFF, "srsrc": ((w1 >> 16) & 0x1F) * 4,
               "ssamp": ((w1 >> 21) & 0x1F) * 4}
        return i
    if top6 == 0x3E:  # EXP
        i = Inst(pc, "EXP", 0, "exp", 8)
        i.f = {"en": w0 & 0xF, "tgt": (w0 >> 4) & 0x3F, "compr": (w0 >> 10) & 1,
               "done": (w0 >> 11) & 1, "vm": (w0 >> 12) & 1,
               "vsrc": [w1 & 0xFF, (w1 >> 8) & 0xFF, (w1 >> 16) & 0xFF, w1 >> 24]}
        return i
    raise DecodeError(f"{pc:04x}: unknown encoding {w0:08x}")


# ------------------------------------------------------------------ PS4 shader footer
@dataclass
class UsageSlot:
    usage: int
    api_slot: int
    start_reg: int
    flags: int


USAGE_NAMES = {0x00: "ImmResource", 0x01: "ImmSampler", 0x02: "ImmConstBuffer",
               0x03: "ImmVertexBuffer", 0x04: "ImmRwResource", 0x05: "ImmAluFloatConst",
               0x06: "ImmAluBool32Const", 0x07: "ImmGdsCounterRange",
               0x08: "ImmGdsMemoryRange", 0x09: "ImmGwsBase", 0x0A: "ImmShaderResourceTable",
               0x0D: "ImmLdsEsGsSize", 0x12: "SubPtrFetchShader",
               0x13: "PtrResourceTable", 0x14: "PtrInternalResourceTable",
               0x15: "PtrSamplerTable", 0x16: "PtrConstBufferTable",
               0x17: "PtrVertexBufferTable", 0x18: "PtrSoBufferTable",
               0x19: "PtrRwResourceTable", 0x1A: "PtrInternalGlobalTable",
               0x1B: "PtrExtendedUserData", 0x1C: "PtrIndirectResourceTable",
               0x1D: "PtrIndirectInternalResourceTable", 0x1E: "PtrIndirectRwResourceTable"}


def parse_footer(code: bytes):
    p = code.rfind(b"OrbShdr")
    if p < 0:
        return None
    version = code[p + 7]
    bits, = struct.unpack_from("<I", code, p + 8)
    info = {
        "offset": p, "version": version, "pssl": bits & 1, "type": (bits >> 2) & 0xF,
        "length": bits >> 8, "chunk_base_dw": code[p + 12], "num_slots": code[p + 13],
    }
    base = p - info["chunk_base_dw"] * 4
    slots = []
    for k in range(info["num_slots"]):
        o = base - (info["num_slots"] - k) * 4
        u, api, reg, fl = code[o:o + 4]
        slots.append(UsageSlot(u, api, reg, fl))
    info["slots"] = slots
    return info


def main():
    code = open(sys.argv[1], "rb").read()
    info = parse_footer(code)
    if info:
        print(f"footer: len {info['length']:#x} type {info['type']}")
        for s in info["slots"]:
            print(f"  usage {USAGE_NAMES.get(s.usage, hex(s.usage)):24} api {s.api_slot:3} "
                  f"s{s.start_reg} flags {s.flags:#x}")
    for i in decode(code, 0, info["length"] if info else None):
        print(i)


if __name__ == "__main__":
    main()
