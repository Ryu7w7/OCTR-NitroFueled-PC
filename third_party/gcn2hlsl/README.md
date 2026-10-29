# gcn2hlsl (the native Vulkan renderer's shader translator)

The game's GCN shader programs are translated to HLSL here, then compiled to SPIR-V with DXC
(`tools/shaders/build_spirv.py`, run on the player's PC from their own game files by
`scripts/native_shaders.py`; nothing derived from the game is shipped).

Source: the `tools/` of the D3D11 port by FrankHUN88453
(github.com/straightarrows/CTR-NitroFueled-PC-d3d11, commit 96b4aa5), MIT License (LICENSE).
Only the modules the shader build needs are kept: `export_shaders`, `setup_shaders`, `gcn2hlsl`,
`gcn`, `iga`, `igz`, `pack_shaders`, `font_glyphs`, `make_glyph_atlas` and `runtime_shaders.json`.

SacredFire changes, all in `gcn2hlsl.py` (found on the native renderer, 2026-10-28):

- the pixel shader's front-face register is a float, +1.0 / -1.0 (it was an integer 1/0: lighting
  of 971 shaders went black or purple);
- 32-bit scalar ops writing vcc_lo set it as a lane mask (black mud on Roo's Tubes);
- VOP3 abs/neg modifiers of v_cndmask_b32 are kept (mirrored tangent frames on trophies);
- slots used only for cube lookups are declared TextureCube (seamless cube filtering);
- a shader that ends with no export discards the pixel, as the hardware does;
- geometry shader ring stores use constant indices (the fur draw: 4.5 ms -> 0.6 ms);
- `GCN2HLSL_NO_FXC=1` skips fxc's D3D11 check (25 minutes -> 17 seconds for every shader).
