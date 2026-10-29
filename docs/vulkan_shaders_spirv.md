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
First in-game run: 6.3M draws, all resolved, 300 shader modules, 386 pipeline states.

## Stage 2: render and depth targets

Each color and depth target bound at a draw gets a `VkImage` and view on the presenter's device (VMA),
keyed by its GPU base address and made again when its size or format changes. The fields read are
the ones the D3D11 port uses:

- **Color** (`Gnm::RenderTarget`): `+0x10` CB_COLOR_BASE, `+0x20` CB_COLOR_INFO (format, number type,
  COMP_SWAP), `+0x4c` width | height << 16. The format mapping follows the port's `ColorTargetFormat`.
- **Depth** (`Gnm::DepthRenderTarget`): `+0x50` DB_Z_INFO, `+0x54` DB_STENCIL_INFO, `+0x58` Z base,
  `+0x80` size. Z_16 without stencil (shadow maps) → `D16_UNORM`, else `D32_SFLOAT_S8_UINT`.

Pipeline keys now use the target formats. Images are the engine's size; render scaling comes with
presentation. Nothing is drawn into them yet, so they hold no data and cost VRAM beside shadPS4's own.
First in-game run: 90 target images (396 MiB), none failed or remade, 402 pipeline states.

## Stage 3: textures and samplers

`gpu/shim/bbport_native_vk_texture.cpp`. For each texture slot a bound VS/PS samples (from
shaders.idx), the engine's T# pointer is read at `ctx+0x320` (VS) or `ctx+0x220` (PS). An unset slot
uses the device's own missing-texture T# at `device+0x580`.

- **Render targets**: a T# whose base is a render or depth target uses that target's image.
- **Other textures**: decoded and laid out as the D3D11 port's `texture.cpp` does (linear aligned
  and 1D thin micro tiling, the GCN mip and pitch rules), read from guest memory and untiled on the CPU
  the first time they are seen. An upload thread copies them into `VkImage`s in batches of up to
  64 MiB, on the graphics queue under `Vulkan::Scheduler::submit_mutex` (which every shadPS4 submit
  and present holds), and leaves them in `SHADER_READ_ONLY_OPTIMAL`.
- **Samplers**: the engine keeps packed sampler keys at `ctx+0x2a0` (VS) and `ctx+0x1a0` (PS). They
  are expanded into an S# as the port does, then into a `VkSampler`: filters, address modes, LOD range
  and bias, border color, comparison, and 16x anisotropy for filtered mipmapped samplers.
- **Budget**: textures stop being made past `BB_VK_TEXTURE_MB` (default 1024), since they sit beside
  shadPS4's own copies until the native path draws.

Not done yet: refreshing textures the CPU rewrites (videos, runtime atlases), 2D-tiled modes,
3D textures beyond the first slice, and the per-slot swizzle constants (`ctr_swz`, b13) the shaders
expect.

First in-game run: 880 textures (538 MiB) uploaded in 312 batches, 14 samplers, none failed. 18% of
texture binds are render targets. The 24 textures not converted are all GPU-written screen-sized
images (tiling 14 = 2D thin, and 0): destinations of the engine's compute copies (`kCopyTexture`,
which the D3D11 port handles in its copy hook). They are handled when copies run natively, not by
the CPU untiler.

## Stage 4: vertex and index data

`FetchVertices` in `bbport_native_vk.cpp` builds each draw's vertex layout as the port's `GetLayout`
does:

- **Matching:** each VS input semantic is matched by usage and usage index (shader header `+0x18` →
  input elements at `+0x30`) to an element of the engine's vertex formats (`ctx+0x520`). That element
  gives its stream slot, byte offset and Gnm data format.
- **Streams:** slot strides are at `ctx+0x610`. Engine buffer records (`{u32 size; u64 address}`) are
  at `ctx+0x4a0` for vertex streams and `ctx+0x5a0` for the index buffer. The index size is at `ctx+0x5c4`.
- **Formats:** integer formats (uint/sint, uscaled/sscaled) are fetched raw, as the translated shaders
  expect, and the shaders convert scaled data themselves.
- **Topology:** fans and quads need generated triangle-list indices, as in the port.

**Improvement over the port:** instead of copying vertex and index data, `bbport_native_vk_buffer.cpp`
imports the game's memory as `VkBuffer`s (`VK_EXT_external_memory_host`, now enabled on the device
when available). Imports use windows of up to 16 MiB, clipped to the committed region around the
data, so the GPU reads vertices and indices in place, as the PS4's GPU does. That needs no per-draw
copy and no write tracking. Memory that will not import falls back to copying; the log reports both.

First in-game run (6800U, AMD Windows driver): the import works, alignment 4096. 112 windows
(936 MiB) cover the race; 94% of vertex streams and all but 0.05% of index ranges were read in place.
The rest came from 2 windows that were never imported: their engine buffer records ran past the end
of the committed memory (about 7 MiB per record), and a window then covered the whole record, which
could not import. Windows now stop where the committed region ends, since a draw reads only vertices
that exist, and failures are logged with the record's address and size. Also counted: 58 vertex
layouts, no fans or quads, no unknown formats. About a third of VS inputs have no matching vertex
element (the port feeds zeros), and many attributes are integer formats feeding float inputs.

The device already enables `VK_EXT_vertex_input_dynamic_state` where supported, so vertex layouts
stay out of the pipeline key at the draw stage.

Second run, with windows clipped to the committed region: 4.2M lookups, all in place; 153 windows
(1.1 GiB), none failed, nothing to copy.

## Stage 5: drawing

Every draw is now recorded into the renderer's own command buffers, into its own target images.
shadPS4 still renders the frame too. `BB_VK_SHOW=1` makes the presenter show the native picture
instead, and `BB_VK_DRAW=0` goes back to stage 4 (counting only). The launchers in the portable
folder are `Run native vulkan.bat`, `... - show its picture.bat` and `... - validation.bat`. The
last one adds the SDK's validation layer, whose messages land in the log.

- **Shaders** (`bbport_native_vk_spirv.cpp`): at load, each module's descriptors move to its
  stage's set (VS 0, PS 1, GS 2), and its bindings are read back for the set layout. Vertex inputs
  fed integer formats are retyped to `uint4`/`int4`, with every load bitcast back to `float4`. The
  translated shaders read such inputs with `asuint()` (D3D11 passes raw bits), and Vulkan requires
  the input type to match the format. One module variant is made per integer-input mask. Checked
  offline: all 3203 modules read; all 500 vertex shaders patch (16 read inputs through access
  chains, handled too) and pass `spirv-val`.
- **Recording** (`bbport_native_vk_gpu.cpp`):
  - Command buffers are submitted when the game submits (`sceGnmSubmit*`), on the graphics queue
    under shadPS4's submit lock, with up to 6 in flight.
  - Constants and generated indices come from a 64 MiB upload ring, split per submission. A full
    ring submits early.
  - Descriptor pools are per submission.
  - Image layouts are tracked per target. Each layout change, and the start of each rendering pass
    (dynamic rendering), goes with one full memory barrier: simple and safe, with finer barriers
    for later.
- **Per draw** (`bbport_native_vk.cpp`):
  - **Constant buffers:** copied from the shader header's vectors (`+0x48` data, `+0x60` sizes,
    `+0x78` slots) into the ring, as the port.
  - **b13 InfoCB:** `ctr_swz` per texture slot (the port's SwizzleCode, using the target's
    COMP_SWAP) and `ctr_vin` per input (1 uscaled, 2 sscaled). Scales are 1 until render scaling.
  - **Descriptor sets:** one per stage, written from the module's bindings. Missing buffers and
    images are null descriptors (`nullDescriptor` is on), which read zeros.
  - **Pipelines:** cached by shaders (and the integer-input variant), attachment formats,
    blend/mask, depth-stencil, cull/face/offset registers and topology. Viewport, scissor, depth
    bias, stencil reference and the vertex layout are dynamic.
  - **Viewport and scissor:** counted bottom-up as in the port, with a negative-height viewport for
    D3D's Y. The scissor is clamped to the rendering area.
  - **Feedback:** a sampled target that is also a color attachment is unbound. A sampled depth
    target is bound read-only, or dropped when the draw writes depth (as the port).
  - **Fans and quads:** indices are generated into the ring.
  - **Not drawn yet:** draws with a geometry shader (2 ES/GS pairs; the ES-to-GS ring comes later).
- **Clears and copies:** `kClearRenderTarget` and `kCopyTexture` are hooked as pass-through stubs
  (`bbport_hook.cpp`, shared with the engine probe). Clears clear our images. Copies between
  same-size formats at mip 0 copy our images, and the destination becomes one of our targets, so
  later draws sample it (the 24 GPU-written textures stage 3 could not convert). Others are logged
  and counted.
- **Display:** the presenter's display-override path (`vk_presenter.cpp`) takes our image of the
  display buffer. It is moved to General layout at each submit, viewed as R8G8B8A8 with the
  presenter's sRGB-ness (our images hold the shader's logical channels, so no swap), and handed
  back in General after the presenter's passes.

Not done yet: geometry-shader draws, render scaling, CPU-written texture refresh, 2D tiling, 3D
and cube textures, depth or format-changing copies, texture-sourced copies, and the image a
presented buffer keeps if its target is remade.
