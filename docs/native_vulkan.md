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
| `Run native vulkan - alone (2 frames ahead).bat` | As alone, with `BB_FRAMES_AHEAD=2` |

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
| `BB_VK_DEFER_SIGNALS=0` | Alone mode: the game's thread waits for the capture thread at each submit, instead of the GPU labels waiting for it (`Run native vulkan - alone (wait at submit).bat`) |
| `BB_VK_SUBMITS_AHEAD=N` | With deferred labels: how many submissions the game may run ahead of the capture thread (1). With no bound, frames came in bursts (40/17/2.5 ms) |
| `BB_ANISO=N` | Forced anisotropy, as shadPS4's (16; 0 or 1 keeps the game's filters) |
| `BB_VK_PREP_WORKERS=N` | Threads that work out each draw's shaders, vertex layout and indices ahead of the capture thread (2; 0: the capture thread does it all) |
| `BB_VK_DECODE_DRAWS=1` | Alone mode: shadPS4 decodes the draw state and draws again (by default it passes over them) |
| `BB_FRAMES_AHEAD=N` | shadPS4's: how many frames its recording thread may run ahead of the GPU at a flip (1). `Run native vulkan - alone (2 frames ahead).bat` tries 2 |
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
  them stale.
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

## Ideas not done yet

1. **Read-modify-write copies.** The engine copies a G-buffer layer, then redraws that layer
   with one full-screen draw sampling the copy. If that draw provably writes every pixel
   without blending, the copy could become a swap. A feedback-loop layout
   (`VK_EXT_attachment_feedback_loop_layout`) could also remove it where the shader reads only
   its own pixel.
2. **GPU bubbles**: about 10–15% idle. In alone mode, shadPS4's recording thread waits at each
   flip until the GPU finishes the frame before (`BB_FRAMES_AHEAD=1`, `PrepareFrame`): about
   20% of a race and 80–90% of the title screen (`BB_WAIT_TRACE=1`, which now works on Windows
   too). Labels queued on that thread wait with it, so the game waits too, then the GPU
   waits for the game. Try `BB_FRAMES_AHEAD=2`. Also check submission boundaries,
   pipeline-not-ready skips and waits on texture uploads.
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
