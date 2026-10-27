# Native Vulkan renderer (experimental)

A second renderer for the port, beside the shadPS4-derived one. Instead of translating the PS4
command buffers, it hooks the game engine's own draw, clear, copy and submit calls and builds
the frame in Vulkan directly, the way a PC engine would. It is off by default.

**Status (2026-10-26): it renders the whole game correctly** (intro, menus, course select,
races, Crash's fur), but it is **about 7% slower than the stock shadPS4 renderer** on the
reference machine: a race runs at about 47 fps against shadPS4's 50.6 on a Ryzen 7 6800U
(680M), plugged in, Turbo mode. Nobody has closed that gap yet. Contributions are welcome.

## Running it

From the portable folder:

| Launcher | What it does |
|---|---|
| `Run original shadPS4 (stock).bat` | The shipped renderer: the reference to compare against |
| `Run native vulkan.bat` | The native renderer draws beside shadPS4, which still draws the picture |
| `Run native vulkan - show its picture.bat` | Both draw; you see the native one |
| `Run native vulkan - alone.bat` | **The real mode**: shadPS4 skips every draw, only the native renderer draws |

Each run writes `logs/<time>.log`, `.frames.csv` (every frame's time) and, with
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
| `BB_VK_RECORD_THREAD`, `BB_VK_SYNC_PIPELINES=1`, `BB_VK_DEFER_COPIES=0`, `BB_VK_VERTEX_COPY=0`, `BB_VK_FULL_BARRIERS`, `BB_VK_RESIDENT`, `BB_VK_TEXTURE_MB`, `BB_VK_NATIVE_SHADERS`, `BB_VK_DRAW` | Comparison and debugging switches; see `gpu/shim/bbport_native_vk.cpp` |

## How it works

Code: `gpu/shim/bbport_native_vk*.cpp`.

Three threads per frame:

1. **Game's render thread** (engine hooks). At each draw it copies what the engine overwrites
   once the call returns: the context, render target and buffer descriptors, the texture
   descriptors (T#s), and the vertex, pixel and geometry shader constants. About 2 µs per draw.
   Anything reached through an engine object pointer must be copied here; it is rewritten in
   place (Bink videos, Crash's fur shells).
2. **Capture thread**: resolves shaders, textures, vertex layouts, pipelines and index data into
   a batch. At each submit the game's thread waits for it, so guest memory is read before the
   GPU-side reuse point.
3. **Recorder thread**: upload ring, layouts, render passes, descriptor sets, command buffers,
   submission.

Also:

- **Pipelines** compile on worker threads. A draw whose pipeline is not ready is left out, and
  the known pipelines are preloaded from the last run.
- **Textures** are untiled and uploaded on their own thread. Guest writes to their pages make
  them stale.
- **Render targets** are our own images, keyed by guest address; their memory is watched too.
- **Texture copies** (the engine's `kCopyTexture`) are deferred until something needs them:
  - copies whose destination is never read are never made;
  - a whole copy whose source is then cleared becomes an image swap.
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

## Ideas not done yet

1. **Read-modify-write copies.** The engine copies a G-buffer layer, then redraws that layer
   with one full-screen draw sampling the copy. If that draw provably writes every pixel
   without blending, the copy could become a swap. A feedback-loop layout
   (`VK_EXT_attachment_feedback_loop_layout`) could also remove it where the shader reads only
   its own pixel.
2. **GPU bubbles**: about 10% idle. Check submission boundaries, pipeline-not-ready skips, and
   waits on texture uploads.
3. **Shared power budget**: the 6800U shares power between CPU and GPU, and the renderer's extra
   threads may lower GPU clocks. Compare GPU clocks against stock shadPS4 in a race.
4. **More PC shaders** for the heaviest translated shaders (`shaders.py`-style per-shader GPU
   time from the trace file). Two hand-written versions of `UberShaderBlended_8c4835d9` were
   slower than the translation, so profile before rewriting.
5. **Compute dispatches** are still run by shadPS4 in alone mode, into its own images. Anything
   the engine produces with compute and later samples is only correct where it does not matter
   today.

## Lessons from building it

- Copy at the draw anything the engine rewrites in place. Late reads gave shaking fur (geometry
  shader constants) and broken videos (texture descriptors).
- Every guest write path has to reach the native caches. Writes shadPS4 took to GPU-modified
  memory once skipped them, and the course previews then drew the title screen's Crash.
- shadPS4 runs every engine texture copy too (with the game's compute shader), so copies alone
  do not explain the gap.
