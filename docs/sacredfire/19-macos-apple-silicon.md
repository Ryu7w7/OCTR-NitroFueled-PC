# macOS on Apple silicon: what the hardware allows (2026-10-19)

Measured with `tools/macos-smoke` on an Apple M2 Pro, macOS 26.2 (25C56), as an x86-64 process
under Rosetta 2. It revisits the "not realistic" verdict of
[10-other-platforms.md](10-other-platforms.md) for the CPU and memory side; the GPU side is open.

## Results

| Need | Result |
|---|---|
| Guest instructions: AVX, AVX2, F16C, BMI1 (andn), LZCNT, MOVBE, POPCNT | All execute correctly. `cpuid` reports none of AVX, F16C, MOVBE, BMI1, LZCNT, AVX2 (only POPCNT): CPU checks must probe by executing, and the ISA rewrite must not trigger on cpuid alone |
| Eboot at 0x800000000 (+0x6000000) | Free |
| Guest memory USER_MIN..USER_MAX (0x1000000000..0xfc00000000) | **0xfc0000000..0x7000000000 is already reserved** (PROT_NONE, tag 0; mapping over it with MAP_FIXED crashed Rosetta). 0x7000000000..0xfc00000000 (560 GiB) is free |
| One page at two addresses (direct memory) | Works in the free range with shm_open + MAP_SHARED\|MAP_FIXED and with mach_vm_remap(copy=FALSE) |
| 4 KiB granularity (sysconf reports 4096; the host uses 16 KiB pages) | 4 KiB-aligned views at 4 KiB offsets work; mprotect of one 4 KiB page leaves its neighbours writable |
| Guest TLS | `gs:[8*key]` reads `pthread_getspecific(key)` on every thread (key 258 -> gs:[0x810]), so the Windows scheme (rewrite `mov rax, fs:[0]` to a gs slot load) carries over |
| Write tracking | SIGSEGV/SIGBUS handler gets the address and rip, unprotects, the write resumes |
| Code patched at run time | Rosetta picks up the patch |

## What it means for a port

- Build everything x86-64 (`-arch x86_64`) and run under Rosetta 2; macOS 15 or later.
- Move the guest window to start at 0x7000000000 on macOS and never MAP_FIXED over ranges the
  process did not claim first (claim with mach_vm_allocate(VM_FLAGS_FIXED), which fails on a used
  range).
- Direct memory: shm_open objects or mach_vm_remap instead of memfd/mremap.
- Unverified: whether the game accepts guest addresses from 0x7000000000 up; Rosetta performance;
  the renderer on MoltenVK (no geometry shaders: fur, 06-fur.md) and the Vulkan extensions the
  shadPS4 core expects.

## Performance and fur (2026-10-22, M1 8 GB)

- **Shaders 7x faster:** the recompiler decorated every float operation `NoContraction`; SPIRV-Cross
  (MoltenVK) turns each into a call to an `[[clang::optnone]]` helper, and one CTR scene shader took
  43 ms a frame. On Apple the decoration is left out
  (`shader_recompiler/backend/spirv/emit_spirv_floating_point.cpp`). Race on Crash Cove: 4 → 27-30
  FPS at 1080p. Found with `BB_SKIP_FILE` (live draw-skip diagnostic in `vk_rasterizer.cpp`).
- **Fur without geometry shaders:** a geometry shader draw runs as two draws
  (`vk_gs_emulation.{h,cpp}`, `emit_spirv_gs_emulation.cpp`). The export stage stores every vertex
  (position + ESGS vec4s) into a buffer by device address, with rasterizer discard. Then the
  geometry stage runs as a *vertex* shader: one invocation per output vertex slot of each triangle;
  it reloads its triangle's three vertices through the index buffer and returns at the `EmitVertex`
  of its slot (unused slots get position 0, an empty triangle). The two passes are in separate
  render passes: a barrier inside one Metal render pass left vertices unwritten (stray fur
  triangles). The emulated stages embed session buffer addresses, so they never go to the shader
  cache. `BB_GS_EMULATION=0` turns it off. Supported: triangle-list input, no GS instancing, no
  stream-out, output strips of whole triangles (CTR's fur shells).
- Shader cache versions bumped (binary 12, meta 10, pipeline key 8): the first launch after the
  update recompiles the shaders (a few minutes).
- **Known gap (Halloween event):** while the Halloween season event is on (its ghosts on the
  tracks), fur is not drawn on macOS; see the next section.

## Open issue: fur during the Halloween event loses the device (2026-10-22)

**Symptom.** With the Halloween season theme (`BB_THEME=halloween`: patches.py turns on the
game's hidden-object event, ghosts on the tracks) a race crashes a few seconds after it starts:
`[mvk-error] VK_ERROR_OUT_OF_DEVICE_MEMORY: Lost VkDevice after MTLCommandBuffer ... (code 9):
Invalid Resource (kIOGPUCommandBufferCallbackErrorInvalidResource)`, then
`vk_swapchain.cpp: AcquireNextImage ... ErrorDeviceLost`. Online the theme comes from the server
(`Season theme from the server: halloween` in the log); offline `theme=halloween` in `bbport.ini`
turns it on. Seen first by a player online on the M1 test Mac.

**Reproduce** (M1, macOS 26.2, MoltenVK 1.4.2 from the Vulkan SDK 1.4.363): copy `bbport.ini`
with `online=0` and `theme=halloween`, start with `BB_CONFIG=<that file>`, pick a furry character
(Polar was used), start a race on Crash Cove and accelerate: it crashes
within the first lap, almost every time. `BB_GS_EMULATION=all` brings the crash back after the
stopgap below.

**What sets it apart.** Only the theme: the fur draws are the same as without it (ES `0x998b09f1`,
GS `0xdcc4d960`, 64-dword ESGS items, i.e. 16 vec4 per vertex; their pixel shaders bind 12-13
BC1/BC3/BC5/RGBA8 textures, tile mode 13). Without the theme the same draws run for whole races.

**Test matrix** (each a full race attempt; "crash" = the device loss above):

| Change | Result |
|---|---|
| Fur emulation off (`BB_GS_EMULATION=0`: the draws are skipped before anything is bound) | no crash |
| Emulation on, the two passes created but nothing bound or recorded | no crash |
| Only the export pass recorded (expansion draw skipped, its resources still bound) | crash |
| Export pass skipped entirely (nothing bound), expansion pass drawn | crash |
| Expansion resources bound, nothing drawn at all | **crash** |
| Same build, theme off | no crash (fur drawn) |
| Indices copied into the emulation's own buffer instead of read from the buffer cache by address | crash (kept: simpler) |
| Arena migration copy skipped / the merge happens on the recording thread (checked) | crash |
| Export pass with no attachments (its pipeline had the draw's depth format) | crash |
| Texture collector off (`BB_TOGGLE_FILE` bit 31) | crash |
| `MVK_CONFIG_USE_MTLHEAP=1` | crash |
| `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` | no crash once, crash the next time |
| Metal API + shader validation (`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`, log mode) | no crash, no validation error |
| `MVK_CONFIG_DEBUG=1` | no encoder named in the error |

**Reading.** With the event on, binding the fur draws' resources is enough: the fragment stage's textures and
buffers get created, uploaded and detiled for the first time (before the emulation these draws
returned before binding). Something in that upload path, or a resource it
frees early, is invalid for Metal; validation hides it (it keeps resources alive and changes
timing), so a use-after-free of an image or buffer is the leading suspicion, not proven.

**Next steps.** Log the texture cache's image creations, uploads and deletions with their
scheduler ticks around the crash; try the fur's textures alone (bind only the fragment stage's
images, then only its buffers); capture the failing command buffer with
`MTLCommandBufferDescriptor.errorOptions = encoderExecutionStatus` (MoltenVK does not set it).

**Stopgap (shipped).** `RecordGsExportPass` skips the emulated fur while `BB_THEME` is
`halloween` (run.sh exports it from `bbport.ini` theme= when the server did not set it), as before
the emulation: no fur during the event, fur the rest of the year. `BB_GS_EMULATION=all` draws it
anyway (to reproduce). Checked on the M1: two full Halloween races without a crash, and fur in races
with the theme off.
