# Game patches (CTR)

`patches/CrashTeamRacing.xml` carries the CUSA13795 1.21 patches from
[illusionyy/PS-Game-Patch](https://github.com/illusionyy/PS-Game-Patch) (authors illusion and
TroyWarez): "60 FPS Unlock", "Disable Motion Blur", "Resolution Patch (1080p for PS4 Pro)".

- In-game menu (Insert or L3+R3), section "Game patches (restart required)": three checkboxes,
  saved to bbport.ini as `patch_60fps`, `patch_no_motion_blur`, `patch_pro_1080p` (0/1). The
  menu offers "Apply and restart game" when a choice differs from the one the game started with.
- At start run.sh (ctr profile) runs `scripts/patches.py --external-only --patches-dir patches
  --title-id CUSA13795 --app-version 01.21 --settings bbport.ini`, which compiles the selected
  patches (plus any `patches.json` selection) into `out/patches.bin`; the executable applies the writes
  to the image after relocation (`--patches`). bbport's own Bloodborne patch logic (frame rate, resolution, effects) was removed afterwards (09-bloodborne-strip.md).
- `BB_PATCHES_DIR` points at another folder of shadPS4-format XML files for this title.
- The 1080p Pro patch only makes sense with `BB_NEO=1`.

## 60 FPS on the Steam Deck (measured 2026-10-09)

- With a 60 Hz virtual vblank the game alternated between 16.7 and 33 ms frames (40-44 FPS in
  races, 50 in menus): a frame slightly late for the vblank costs a whole extra one. run.sh now
  sets `BB_VBLANK_HZ=480` when `patch_60fps=1`; menus then hold 57-60 FPS.
- Races stay at 37-45 FPS. In the default memory model the command-processing thread is the
  limit (idle 5%, ~1,400 draws/frame at ~10 µs each plus write-tracking faults). The experimental
  PC model (`BB_PC_MODEL=1`) frees that thread (idle 40%, no write faults) but commits the whole
  direct-memory arena as GPU-visible memory: amdgpu GTT 5.8 GB (1.8 GB in the default model),
  which on the Deck's shared 16 GB pushed 15 GB into swap; the game then waited 18.5 ms per frame
  for the previous frame. Not usable here without a bounded GTT budget.
- Threads during a race (old model): no single thread saturated; machine ~80% busy on 8 threads.

## Pro 1080p patch: off-by-one in the published file (2026-10-09)

With only `patch_pro_1080p=1` the game faulted at boot at guest offset 0x13a9db3 (signal 11,
address 0x803b8815f), the patch address itself. Disassembly of out/eboot.elf around it:

```
0x17a9dac  cmpb $0x0, 0x17440f5(%rip)   ; the "is PS4 Pro" flag
0x17a9db3  0f 85 8f 08 00 00  jne +0x88f ; original
0x17a9db3  84 85 8f 08 00 00  test %al, 0x88f(%rbp)  ; with 0x84 written at 0x17a9db3
```

The patch means to turn the `jne` into a `je` (0F 85 to 0F 84), so the byte belongs at
0x17a9db4. patches/CrashTeamRacing.xml now uses that address; the upstream file
(illusionyy/PS-Game-Patch) still has 0x017a9db3 and would fault the same way in shadPS4.

With the corrected patch and PS4 Pro mode (`BB_NEO=1`, the launcher's switch) the game renders
at 1920x1080 and the GPU library asserted in `TextureCache::ResolveOverlap` ("Unresolvable
image overlap with equal memory address"): the same RGBA8 render target was sized 0x7f8000
bytes from a texture descriptor (1920x1088, 8-row alignment) and 0x870000 from the colour
buffer's slice register (64-row macro-tile alignment). Heights that are not a multiple of 64
hit this, and 1080 is the first one CTR uses. The resolver's padding-only rule (formerly for a
differing pow2 flag only) now also covers identical metadata with a different padded size and
reuses the cached image. Pro mode + 1080p patch then runs at 30 FPS (logs/run39-play.log).
