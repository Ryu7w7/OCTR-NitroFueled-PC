# GPL-2.0-or-later. Source for the GUI VFX Line in 16x10.xml.
# Assemble with clang -c; its .text bytes must equal that Line's Value.
# Replaces the 150-byte normalization block in igGuiPlaceableBolt::update.
# rdi: inverse GUI camera; rsi = rdx: three in-place corner vectors; ecx = 3.
# xmm6.x = 2 / canvas width; xmm7.x = -2 / canvas height;
# xmm2.x = -1; xmm3.x = 1; xmm4 = (0,0,0,1); xmm5.x = depth.
# Normalize each corner before unprojection, compensating the 10/9 camera X.
# The caller overwrites xmm4/xmm6 before their next uses. GPRs stay unchanged.
.intel_syntax noprefix
.text
gui_vfx:
    vunpcklps xmm6, xmm6, xmm7
    vmovlhps xmm6, xmm6, xmm4
    vmulss xmm6, xmm6, [rip + factor]
    vinsertps xmm4, xmm4, xmm2, 0
    vinsertps xmm4, xmm4, xmm3, 0x10
    vinsertps xmm4, xmm4, xmm5, 0x20
    vmulss xmm4, xmm4, [rip + factor]
    # Load XY only: discarded input ZW may be uninitialized or non-finite.
    .irp offset, 0, 0x10, 0x20
    vmovq xmm0, qword ptr [rsi + \offset]
    vmulps xmm0, xmm0, xmm6
    vaddps xmm0, xmm0, xmm4
    vmovups [rdx + \offset], xmm0
    .endr
    jmp end
    .fill 146 - (. - gui_vfx), 1, 0x90
factor:
    .float 0.9
end:
    # Continue into the retained native matrix-transform call.
