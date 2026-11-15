# Native Vulkan renderer (experimental)

A second renderer for the port, beside the shadPS4-derived one. Instead of translating the PS4
command buffers, it hooks the game engine's own draw, clear, copy and submit calls and builds
the frame in Vulkan directly, the way a PC engine would. It is off by default.

**Status (2026-10-26): it renders the whole game correctly** (intro, menus, course select,
races, Crash's fur), but it is **about 7% slower than the stock shadPS4 renderer** on the
reference machine: a race runs at about 47 fps against shadPS4's 50.6 on a Ryzen 7 6800U
(680M), plugged in, Turbo mode. Nobody has closed that gap yet. Contributions are welcome.

## Running it

In the launcher, **Graphics engine: Native Vulkan (experimental)** (`bbport.ini` `renderer=native`).
The first start builds its shaders from the player's game (`scripts/native_shaders.py`, a few
minutes). It then runs in the real mode: shadPS4 skips every draw and the native renderer draws
the frame alone (`BB_NATIVE_VK=1 BB_VK_SHOW=1 BB_VK_ALONE=1 BB_VK_NATIVE_ONLY=1`). Without
`BB_VK_NATIVE_ONLY`, shadPS4's rasterizer stays and flips go through its thread (the older
"alone" mode; `BB_VK_SHOW=0` draws with both and shows shadPS4's picture).

Each run writes its log with the renderer's statistics every 5 seconds and, with
`BB_VK_PASS_TRACE=1`, `.trace.txt` (GPU time of every pass and draw for one submission in 1000).

### Switches

| Variable | Meaning |
|---|---|
| `BB_NATIVE_VK=1` | Turn the native renderer on |
| `BB_VK_SHOW=1` | Show its picture instead of shadPS4's |
| `BB_VK_ALONE=1` | shadPS4 skips its draws (needs `BB_VK_SHOW=1`) |
| `BB_VK_SHADERS=<dir>` | Where translated SPIR-V, pipeline caches and native shaders live |
| `BB_VK_PASS_TRACE=1` | Per-pass and per-draw GPU timing into the trace file |
| `BB_VK_CAPTURE_THREAD=0` | Capture draws on the game's thread (old behaviour) |
| `BB_VK_DEFER_SIGNALS=0` | Alone mode: the game's thread waits for the capture thread at each submit, instead of the GPU labels waiting for it (`Run native vulkan - alone (wait at submit).bat`) |
| `BB_VK_SUBMITS_AHEAD=N` | With deferred labels: how many submissions the game may run ahead of the capture thread (1). With no bound, frames came in bursts (40/17/2.5 ms) |
| `BB_ANISO=N` | Forced anisotropy, as shadPS4's (16; 0 or 1 keeps the game's filters) |
| `BB_VK_PREP_WORKERS=N` | Threads that work out each draw's shaders, vertex layout and indices ahead of the capture thread (2; 0: the capture thread does it all) |
| `BB_VK_SERIALIZE_GS=0/1` | Pipelines with a geometry shader compile one at a time per GS (default on the NVIDIA proprietary driver, whose 610.88 shader compiler crashed with two threads building the same fur ES+GS; 0: off, 1: on for every driver). Other pipelines still compile in parallel |
| `BB_VK_DECODE_DRAWS=1` | Alone mode: shadPS4 decodes the draw state and draws again (by default it passes over them) |
| `BB_FRAMES_AHEAD=N` | shadPS4's: how many frames its recording thread may run ahead of the GPU at a flip (1). Only without `BB_VK_NATIVE_ONLY`: in the launcher's mode the flip prepares its frame itself (`Presenter::PrepareNativeFrame`) and the bound is the presenter's frames instead |
| `BB_VK_PIPELINE_WAIT_MS=N` | A draw whose shaders are still loading or whose pipeline is still compiling waits up to N ms for them, at most 4N ms a second (10 by default; 0: it is left out at once). Fewer missing objects on a first run: on an RTX 5080 the waits took about 1 ms each and caused no hitch |
| `BB_VK_RECORD_THREAD`, `BB_VK_SYNC_PIPELINES=1`, `BB_VK_DEFER_COPIES=0`, `BB_VK_VERTEX_COPY=0`, `BB_VK_FULL_BARRIERS`, `BB_VK_RESIDENT`, `BB_VK_TEXTURE_MB`, `BB_VK_NATIVE_SHADERS`, `BB_VK_DRAW` | Comparison and debugging switches; see `gpu/shim/bbport_native_vk.cpp` |

## How it works

Code: `gpu/shim/bbport_native_vk*.cpp`.

Threads per frame:

1. **Game's render thread** (engine hooks). At each draw it copies what the engine overwrites
   once the call returns: the context, render target and buffer descriptors, the texture
   descriptors (T#s), and the vertex, pixel and geometry shader constants. About 2 µs per draw.
   Anything reached through an engine object pointer must be copied here; it is rewritten in
   place (Bink videos, Crash's fur shells).
2. **Prep workers** (2): the part of a draw that does not depend on the draws before it, ahead
   of the capture thread, in any order: shaders, vertex layout, and the indices (copied, and the
   highest one found). A draw's batch keeps its capture until it is recorded, so the recorder
   uploads the constants and indices from the capture itself, with no second copy.
3. **Capture thread**: in order, resolves shaders, textures, vertex layouts, pipelines and index data into
   a batch. Guest memory must be read before the game reuses it, which it does once it sees the
   submission's end-of-pipe label. In alone mode those labels (written by shadPS4's command
   processor, `liverpool.cpp`) wait for the capture thread, so the game's thread goes on with
   the next frame at each submit; otherwise the game's thread waits for it at each submit. Before
   this, the game's thread waited about 3.5 ms per race frame there, which matched the GPU's
   idle time.
4. **Recorder thread**: upload ring, layouts, render passes, descriptor sets, command buffers,
   submission.

In alone mode shadPS4's command processor passes over the draw state (context registers,
graphics shader registers) and the draws, and still runs fences, flips, copies and compute.

Also:

- **Pipelines** compile on worker threads. A draw whose pipeline is not ready is left out, and
  the known pipelines are preloaded from the last run.
- **Textures** are untiled and uploaded on their own thread. Guest writes to their pages make
  them stale. Upload batches are limited to 4 MiB of queued texture data (one larger texture
  still uploads whole). The recorder waits until a texture's batch is submitted; smaller
  batches keep an early texture from waiting for a whole 64 MiB streaming burst to be untiled.
  This targets the texture waits within the recorder stalls reported in issue #224. Validation
  on the affected Intel N100 is still needed; loading a single texture can still stall.
- **Render targets** are our own images, keyed by guest address; their memory is watched too.
- **Texture copies** (the engine's `kCopyTexture`) are deferred until something needs them:
  - copies whose destination is never read are never made;
  - a whole copy whose source is then cleared becomes an image swap.
- **Samplers** get shadPS4's forced anisotropy and no more: mipmapped samplers with a bilinear
  min filter, not depth reads, and the game's mip filter kept (it used to make point-mip
  samplers trilinear too).
- **Shaders**: about 3,200 translated from the game's PS4 shaders, plus four written for the PC
  (`gpu/shim/native_shaders/`): fur, fur blur, the standard G-buffer pass, the blended track
  shader.

## Where the time goes (race, about 47 fps)

- **Game's thread**: no longer the limit. About 1.5–2.5 µs per draw, about 80 ms a second
  waiting for the capture thread at submits.
- **GPU**: busy 850–900 ms of every second, so it is the limit. Per frame:
  - main 1080p passes: about 13 ms;
  - 960×540 passes: about 3 ms;
  - four 1080p RGBA8 copies and one depth copy: about 1.5 ms;
  - shadow maps and post-processing: the rest.
- **Bubbles**: the GPU is idle about 10% of the time.

## A fast PC (RTX 5080, 2560x1440, 2026-11-08)

The numbers above are from the 680M, where the GPU is the limit. On a desktop GPU it is not: in
a race at 327 fps uncapped (Mailbox, 1440p PS4 Pro path), the GPU was busy 279 ms a second and
the game's thread 62 ms, while the presenter's swap thread (`bb:Present`) took 93% of a core. At
120 fps the GPU was busy 100 ms a second. Each run now logs where the presenter's time goes:

    Presenter: per second: 326.8 presents (Mailbox, 3 images); acquire .. ms, record .. ms,
    submit .. ms, driver present .. ms; flips waited .. ms for a free frame (../s)

On that PC the line read `326.0 presents (Mailbox, 4 images); acquire 1 ms, record 4 ms,
submit 9 ms, driver present 922 ms`: the driver's `vkQueuePresentKHR` blocks about 2.8 ms each
time, so the frame rate is the display's pacing (a 360 Hz monitor with the NVIDIA driver's
G-Sync/low latency cap comes to about 326), not the renderer. 3 or 4 swapchain images made no difference.
The ideas below lower GPU time, which matters on the 680M and the Steam Deck, not for frame rate
on such a PC.

## DLSS and HDR

**DLSS** (launcher: Upscaling, NVIDIA DLSS; or `BB_VK_DLSS=1`) runs in the native renderer itself
(`gpu/shim/bbport_native_vk_dlss.cpp`), with the same DLLs as under shadPS4
(`sacredfire_dlss.dll` and `nvngx_dlss.dll` in `bin\`; without them the log says "not available"
and nothing is jittered). It takes the finished scene the display pass stretches, before the HUD:

- the G-buffer pass is the draw with exactly four colour targets and a depth (D32S8, velocity in
  colour 3, viewport z 0-0.8, OpenGL clip depth);
- the camera's motion comes from the scene draws' vertex constants, voted across the pass:
  400/464/528 bytes = previous MVP at float 0, projection at 32, model-view at 48
  (prevMVP * inverse(MV)); the PS4 Pro path's 528 bytes = view at 32, projection at 48, previous
  view at 64 (projection * previous view * inverse(view));
- moving objects stay still on the screen (as under shadPS4; `BB_FSR3_MV` tries decodings of
  CTR's velocity target); `BB_FSR3_NO_JITTER` turns the jitter off;
- draws into last frame's scene depth (not full-screen quads) are moved by a Halton(2,3) jitter,
  only when DLSS took the frame before;
- menus draw their 2D into the scene image over a small 3D scene: a frame whose biggest G-buffer
  pass has fewer than `BB_VK_DLSS_MENU_DRAWS` (100) draws is shown as drawn (the character
  select has 42, a race 380-1700). With DLSS on them, the waving flag, pulsing text and the
  results screen's scrolling background were doubled or smeared, even without history.

The log line `Native Vulkan: DLSS: N frames/s, N upscaled, N without a scene; N as a scene, N menus
left as drawn` says what happened. On an RTX 5080 at 3840x2160: DLAA ~225 fps; 2560x1440 to
2160p (Quality) 260-326 fps. Rendering at 2160p for a 1440p display is already supersampled,
so there DLSS off is faster and looks as good.

**HDR** (launcher: The game's HDR; or `BB_HDR=1`, with Windows HDR on) is the game's own HDR10
output: the scene before the HUD becomes R16G16B16A16Sfloat (DLSS takes it as linear HDR) and
the display buffers A2B10G10R10 (PQ). The game asks the system for no HDR calibration, so the
post-process pass can fit its PQ picture to the display: `BB_HDR_PEAK=nits` rolls off above 75%
of the peak (launcher: HDR peak brightness), `BB_HDR_GAIN` scales it (launcher: HDR paper white,
100% = the game).

## Ideas not done yet

1. **Read-modify-write copies.** Partly done: the reflection blends' screen passes
   (`OwnPixelPasses`) read their targets in place with `VK_EXT_attachment_feedback_loop_layout`
   (`copies ... read in place` in the log), a whole copy whose source is then cleared becomes
   an image swap (`swapped instead`), and a depth copy sampled as the depth target is not made
   (`read from the depth target`). Left: the engine copies a G-buffer layer, then redraws that
   layer with one full-screen draw sampling the copy; if that draw provably writes every pixel
   without blending, the copy could become a swap too. Find which copies remain with the trace
   file (`copy 2560x1440 R8G8B8A8Unorm`: two a frame, about 15 µs each on an RTX 5080).
2. **GPU bubbles**: about 10–15% idle on the 680M. In the launcher's mode the flip handler waits
   for a free presenter frame and its fence (`Presenter::GetRenderFrame`), now logged as "flips
   waited"; `BB_FRAMES_AHEAD` does not apply there (see Switches). Also check submission
   boundaries, pipeline-not-ready skips (`draws left out while compiling`; the known pipelines are
   preloaded from the last run, and a draw waits a little for new ones, `BB_VK_PIPELINE_WAIT_MS`) and waits on
   texture uploads (`waited for their upload`).
3. **Shared power budget**: the 6800U shares power between CPU and GPU, and the renderer's extra
   threads may lower GPU clocks. Compare GPU clocks against stock shadPS4 in a race.
4. **More PC shaders** for the heaviest translated shaders (`shaders.py`-style per-shader GPU
   time from the trace file). Two hand-written versions of `UberShaderBlended_8c4835d9` were
   slower than the translation, so profile before rewriting.
5. **Compute dispatches** would still be run by shadPS4 in alone mode, into its own images. In
   the launcher's mode (`BB_VK_NATIVE_ONLY`) the log shows `shadPS4 compute dispatches: none`
   in menus and races: nothing to move today. Anything the engine produces with compute and
   later samples would only be correct where it does not matter.

## Lessons from building it

- Copy at the draw anything the engine rewrites in place. Late reads gave shaking fur (geometry
  shader constants) and broken videos (texture descriptors).
- Every guest write path has to reach the native caches. Writes shadPS4 took to GPU-modified
  memory once skipped them, and the course previews then drew the title screen's Crash.
- shadPS4 runs every engine texture copy too (with the game's compute shader), so copies alone
  do not explain the gap.
