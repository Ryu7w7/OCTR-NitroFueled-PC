# Resolution in CTR (v0.3 work, 2026-10-12)

## The frame (BB_CAPTURE_TRIGGER captures, race on Crash Cove and the main menu)

- G-buffer: one pass, four RGBA8 targets plus D32S8 depth, all 1920x1080 at 1080p, with
  **2x MSAA** (base PS4 and Pro mode alike).
- Then compute lighting/SSAO (aabd27be, ded6d46b), cascaded shadows (D16 1024x1024 and
  2048x8192), a half-resolution chain, the opaque/forward pass into an RGBA8 scene target
  (35c12c56), transparents (d6549c65 / 6f684a99), a bloom pyramid (8e5b41f8, 960x540 down to
  7x4 and back), the composite (7491f3f5) and two final passes (bc352b2a, 4c414497).
- The HUD is drawn straight into the display buffer, about 50 draws (dc45a169 first).

## Why bbport's live resolution path does nothing here

bbport (Bloodborne) starts its "scene" at a G-buffer pass with 5+ targets and only scales
single-sample targets. CTR's G-buffer has 4 targets (BB_GBUFFER_MRT=4 now covers that) and is
2x MSAA, so `SceneTargets::Eligible` rejects its depth and nothing is ever scaled. CTR's lighting
also runs in compute at the guest size. Supporting MSAA proxies would be a large change for
little gain. The frame-start detection also fired on every HUD draw; it now fires on the first
draw into a display buffer only.

## The game's own resolution switch

The display setup at 0x17a9d90 (1.21, firmware 5.05 build; same in the 6.72+ build) reads the
output size from the game's settings and, when the "is PS4 Pro" flag is set (the branch the
community 1080p patch flips at 0x17a9db4), runs 0x17aa648:

```
17aa648  add  %ecx,%ecx                 ; output width  x2 (3840)
17aa64a  mov  %ecx,0x540(%r14)
17aa651  mov  0x544(%r14),%r8d
17aa658  add  %r8d,%r8d                 ; output height x2 (2160)
17aa65b  mov  %r8d,0x544(%r14)
17aa662  mov  0x48(%r12),%eax           ; render scale from the settings (float)
17aa667  mov  %eax,0x548(%r14)
```

The value at +0x548 is the scene render scale relative to 1920x1080: the Pro default gives
2560x1440 (about 1.333). `mov 0x48(%r12),%eax` and `mov $imm32,%eax` are both five bytes, so a
patch can set any scale (signature `41 8b 44 24 48 41 89 86 48 05 00 00`):

| value at 0x17aa662 | scene | output | result on an RTX 3070 |
|---|---|---|---|
| (game default) | 2560x1440 | 3840x2160 | 88 FPS |
| `b80000803f` (1.0) | 1920x1080 | 3840x2160 | works |
| `b80000c03f` (1.5) | 2880x1620 | 3840x2160 | 87-106 FPS, clean |
| `b800000040` (2.0) | - | - | game OOM at boot: `OOM [1]{7,Garlic}` with the stock GPU heap |
| 2.0 + GPU heap 1792 MiB | 3840x2160 | 3840x2160 | 97-110 FPS (limit 144), clean, BB_DMEM_MB=7168 |

PS4 Pro mode must be on and the 1080p Pro patch off. The 4K display buffer is scaled to the
window by the presenter, so on a 1080p monitor higher scales act as supersampling.

## The game's GPU heap ("Garlic", vramB)

0x1900330 allocates the game's two GPU heaps with sceKernelAllocateDirectMemory (2 MiB
alignment): Onion (type 0) and Garlic (type 3), then maps them. Its caller at 0x1fbb143 takes
the sizes from `ram:/alchemy.xml` keys `Gfx/@vramASize` (default 0x10000000, 256 MiB) and
`Gfx/@vramBSize` (default 0x20000000, 512 MiB; on the Pro, after sceKernelIsNeoMode, the
default is set at 0x1fbb938: `movl $0x3c000000,-0x38(%rbp)`, 960 MiB). The size is read back
with movslq, so it must stay below 0x80000000. Native 4K with 2x MSAA did not fit in 960 MiB;
0x70000000 (1792 MiB, byte 0x70 at 0x1fbb93e) does. More direct memory than the Pro's 5568 MiB
is given with BB_DMEM_MB.

## Internal resolution setting (render_res)

bbport.ini `render_res` (launcher: Settings > Display > Internal resolution; in-game menu:
Internal resolution + Apply and restart) picks 480, 720, 1080, 1440, 1620 or 2160. Above 1080 it
runs the Pro path with the scales above and a 4K output; 720 and 480 use scales 2/3 and 4/9 and
also drop the Pro path's output doubling (`add %ecx,%ecx` at 0x17aa648 and `add %r8d,%r8d` at
0x17aa658 become NOPs), so the picture stays 1920x1080. All six tested on an RTX 3070
(2026-10-12). The game sizes its targets at start, so a change needs a restart (the menu does it).

## Linux desktop scaling (SDL window)

`gpu/shim/window.cpp` sets `SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN=true` so
the Vulkan drawable uses physical pixels on scaled desktops, avoiding compositor upscaling
that can blur the final image. `SDL_GetWindowSizeInPixels` supplies the drawable extent;
`SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` updates it, and the presenter recreates the swapchain
on an extent mismatch. Startup logs report logical size, pixel size and pixel density.
The guest's internal resolution and Pro render scale remain unchanged.
