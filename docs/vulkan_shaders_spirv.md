# Milestone 2, shader step: GCN → HLSL → SPIR-V for 1.21

The native Vulkan path reuses the D3D11 port's offline shader translator
([CTR-NitroFueled-PC-d3d11](https://github.com/straightarrows/CTR-NitroFueled-PC-d3d11), `tools/gcn2hlsl.py`)
and compiles its HLSL to SPIR-V with DXC. `tools/shaders/build_spirv.py` runs the whole chain and keeps
every intermediate.

```
python tools/shaders/build_spirv.py <d3d11 port>/tools <CUSA13795 1.21 dump> out/shaders
```

Needs Windows (gcn2hlsl checks each shader with `d3dcompiler_47.dll`, as the D3D11 port does) and the
Vulkan SDK's `dxc.exe` (`VULKAN_SDK` or `--dxc`). `--only <substr>` limits the run to matching shaders.
Steps 1–3 are reused when their output exists; delete `out/shaders/gcn` or `out/shaders/hlsl` to redo them.
Output is game-derived and stays under `out/` (git-ignored).

## Result on the 1.21 dump (2026-10-25)

| step | result |
|---|---|
| `archives/shaders_ps4.pak` | 87 .igz files; the pak is byte-identical in 1.00 and 1.21 |
| export | 4469 shaders, 2999 unique, plus 7 eboot-resident shaders (below) |
| gcn2hlsl + fxc | 3203/3203 compiled, 0 unsupported |
| DXC `-spirv` | 3203/3203 (2701 ps, 498 vs, 2 es, 2 gs), all pass `spirv-val --target-env vulkan1.1` |

Time on the 6800U: about 13 min for translation, 2 min for DXC. SPIR-V totals 63 MB.

### Eboot-resident shaders

The port's `runtime_shaders.json` gives 1.00 file offsets in eboot.bin. In 1.21 the same shaders (same
hashes) moved; the script finds them through their `OrbShdr` footer:

| shader | 1.00 | 1.21 |
|---|---|---|
| Rt_74033caa94e14117_vs | 0x1b22404 | 0x1c309c4 |
| Rt_6d0ae01fb1b6a3de_ps | 0x1b22650 | 0x1c30c10 |
| Rt_06414427fa62da56_ps | 0x1b23310 | 0x1c318d0 |
| Rt_09baa8a35ea69dcc_vs | 0x1b2b8c0 | 0x1c39e80 |
| Rt_e0a8516c3b36cb92_vs | 0x1b2c318 | 0x1c3a8d8 |
| Rt_68992888de7632ba_ps | 0x1b2c5b8 | 0x1c3ab78 |
| Rt_3c0efb6e370b0c10_ps | 0x1b2ce0c | 0x1c3b3cc |

## Vulkan conventions the renderer must follow

- **Descriptor bindings**, all set 0: `b#` → `#`, `t#` → `64+#`, `s#` → `128+#`, `u#` → `192+#`.
  `cbuffer CTRSwizzle : register(b13)` (the port's texture swizzle and vertex input fix-ups) lands at binding 13.
- **Stage I/O locations are pinned** before DXC: `VARYINGn` → location `n-15`, `ATTRIBn` → location `n`.
  DXC otherwise numbers them by declaration order, and stages declare only the varyings they use.
  For example, `TiledLight_46a74525_ps` reads VARYING17/18 without 16, which would otherwise land at
  locations 0/1 and read the wrong vertex outputs.
- **Shader metadata**: `out/shaders/hlsl/shaders.idx` is the port's runtime index. It holds `S <fnv1a64 of the
  GCN program> <name> <kind>`, then `T`/`M`/`B`/`I` lines for textures, samplers, cbuffers and vertex inputs.
  The renderer looks shaders up by hashing the program the engine binds, exactly as `port/src/gfx/shaders.cpp`
  does.
- **Clip space**: D3D and Vulkan share 0..1 depth. Y is flipped, so use a negative-height viewport
  (`VK_KHR_maintenance1`, core in 1.1) rather than `-fvk-invert-y`, so the same SPIR-V serves all passes.
- **ES/GS**: two of each. The port emulates the ES→GS ring in HLSL, so a Vulkan geometry stage is needed.
  If a device lacks `geometryShader`, those draws need a fallback.

## Native renderer, stage 1: front end (`BB_NATIVE_VK=1`)

`gpu/shim/bbport_native_vk.cpp`, enabled with `BB_NATIVE_VK=1` and `BB_VK_SHADERS=<out/shaders>`.
It is called from `sceGnmDrawIndex` and `sceGnmDrawIndexAuto`. At each draw it:

- reads the engine context through the device global,
- resolves the bound VS/PS (and GS behind an ES) by hashing the GCN program, as the D3D11 port does,
- loads each matched shader's SPIR-V as a `VkShaderModule` on the presenter's device,
- counts the distinct pipeline states (shaders, target layout, blend/depth/stencil/raster registers,
  primitive and index type).

Programs without a translation are logged once each. A summary is logged every 5 s.
Nothing is drawn by it yet: shadPS4 still renders. Next: render targets and depth (engine target →
VkImage), then textures and samplers, vertex/index fetch, and the draw itself.
