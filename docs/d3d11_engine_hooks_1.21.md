# Engine render hooks: 1.00 → 1.21 address map

Where the D3D11 port ([CTR-NitroFueled-PC-d3d11](https://github.com/straightarrows/CTR-NitroFueled-PC-d3d11),
`port/src/gfx/engine.h`) hooks the Alchemy render layer, and the same points in the 1.21 eboot this
fork runs. Addresses are eboot virtual addresses (image offsets, base 0).

Builds compared (both UP0002-CUSA13795_00-CRASHTEAMRACING1):

| | eboot.bin size | SHA-1 (first 16) |
|---|---|---|
| 1.00 (what the D3D11 port targets) | 50,922,182 | `fb2619da53542ad3` |
| 1.21 (update, firmware 6.72+ build) | 52,783,970 | `3c4d003401e91566` |

## Address map

| Name (engine.h) | What | 1.00 | 1.21 | How it was found |
|---|---|---|---|---|
| `kClearFunction` | `kClearRenderTarget(ctx, flags, float4* color, stencil, float depth)` | `0x1306b90` | **`0x13c9e50`** | first 60 instructions (239 bytes) identical with RIP/call displacements masked; one hit in .text |
| `kCopyTextureFunction` | `kCopyTexture(ctx, T# src, T# dst, s32 p[8])` (compute copy) | `0x1307f10` | **`0x13cb1d0`** | same, 253 bytes; one hit |
| `kDeviceGlobal` | `igPS4GraphicsDevice*` global | `0x26e7920` | **`0x2aebce0`** | 19 functions that reference it matched uniquely in 1.21; all 19 point to this address |

Both hooks overwrite 12 bytes at the function start (`mov rax, imm64; jmp rax`); the 1.21
prologues are identical to 1.00 (`push rbp; mov rbp, rsp; push r15..rbx; ...`), so the patch is safe
as is.

Draws need no address: the port intercepts `sceGnmDrawIndex` / `sceGnmDrawIndexAuto` by NID, and the
NIDs don't change between builds.

## Struct offsets (`DEV_*`, `CTX_*`, `SH_*`, `HDR_*`)

**Unchanged in 1.21, as far as the code shows.** The device/context implementation lives in
1.00 `0x12f0000–0x1320000` (1.21: shifted by about +0xc32c0, not uniformly). Of its 624 functions:

- 153 match 1.21 exactly (only RIP/call displacements differ), 451 are short stubs that match in
  several places (ambiguous, not counted as evidence);
- the rest were paired by their position between exact matches, and the set of struct
  displacements each one uses (`[reg + disp]`, excluding stack and RIP) was compared:
  **465 pairs use exactly the same offsets**, including the two large functions that read the
  context's render state (1.00 `0x13021b0`, size 0x49e0, and `0x1308f40`, size 0x4930);
- 2 functions gained accesses in 1.21 without losing any: 1.00 `0x130e530` → 1.21 `0x13d1790`
  (new `+0xc`, `+0x50`) and `0x1311a70` → `0x13d4d50` (new `+0x3`). They add code, they don't move
  fields.

So `CTX_*` (viewport `0x0c` … instanced VBs `0x650`), `SH_*` and `HDR_*` can be used unchanged.
`DEV_*` (`DEV_CONTEXT = 0x5b0`, back buffer `0x508/0x510/0x540/0x544`, `0x580`) live in the same
code region and are expected to be unchanged too, but the global is reached through a `lea` and the
field read happens further on, so this check did not see a `+0x5b0` load directly. **Confirm at
runtime:** read `*(base + 0x2aebce0)`, then `+0x5b0`, and check that the pointer is the `ctx` that
`kClearRenderTarget` receives in `rdi` (the D3D11 port's `gfx/probe.cpp` does this dump).

## Not mapped (outside the renderer)

The D3D11 port also uses UI addresses for its mouse and Video settings page
(`mouse_ui.cpp`: `kGuiInputVtable`, `kPlaceableContains`; `settings_menu.cpp`: `kClonePlaceable`,
`kSetFocus`, `kSetText`, `kStringPool`, `kInternString`, `kSelector*`, `kNavItemVtable`,
`kGlobalSelectorVtable`). A Vulkan backend doesn't need them; the same method can map them later.

## Shaders

The D3D11 port builds its HLSL from the player's own dump at first start (`tools/setup_shaders.py`,
`tools/gcn2hlsl.py`), so it should pick up 1.21's shaders from a 1.21 dump. Not tested: any shader
1.21 adds or changes goes through the same translator, and `DumpMissingShader` catches those it
doesn't have.

## Reproducing

The scripts are in [tools/hookmap](../tools/hookmap) and need `capstone` and `numpy`.

```
python -I self2elf.py <1.00>/eboot.bin e100.elf          # tools/self2elf.py from the D3D11 repo
python -I self2elf.py <1.21>/eboot.bin e121.elf
python -I tools/hookmap/hookmap.py e100.elf e121.elf     # clear/copy hooks, references to the global
python -I tools/hookmap/global.py e100.elf e121.elf      # the global's 1.21 address, by vote
python -I tools/hookmap/changed.py e100.elf e121.elf     # struct offsets of the device/context code
```
