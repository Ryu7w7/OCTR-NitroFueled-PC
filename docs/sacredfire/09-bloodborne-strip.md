# Removing the Bloodborne profile (2026-10-09)

Removal commit: 9773394 (its parent is the last tree with both profiles).
Scope chosen: profile and tooling only; bbport's GPU modules stay in `gpu/` (inert for CTR).

## Removed

| piece | was |
|---|---|
| `patches/Bloodborne.xml` and the built-in patch logic in `scripts/patches.py` (FPS presets, resolution/UI viewport writes, effect switches, model LOD, Intel tone-map fix) | Bloodborne 1.09 addresses |
| `launcher/bbport_launcher.py`, `launcher/bbport_assets.py` (FSR 4.1.1 asset checks) | the Bloodborne GTK launcher |
| `tools/fsr4cap/`, `tools/fsr4_*.{sh,pl}`, `tools/Fsr4SpirvCrossFixes.pm`, `tools/fetch_fsr4_assets.sh`, `tools/mv_check.py` | FSR 4 capture/optimisation tooling |
| run.sh: the Bloodborne branch (render scaling, live resolution, FSR asset lookup and optimisation) | |
| `src/runtime_savedata.c` sound-flag hack, `src/runtime_pad.c` libc.prx malloc_stats probe, "Hunter" user name | profile-gated runtime pieces |
| GPU menu: the "Game effects" section and the Bloodborne title; upscaler/object motion now default off for every profile | `gpu/shim/bbport_overlay.cpp`, `bbport_settings.cpp` |
| `packaging/default.nix`: dxil-spirv and fsr4cap.exe derivations, fsr4_shaders/fsr4_411 bundling | |
| tests `test_run_settings.py`, `test_upscaler_assets.py`, `test_fsr4cap_dll_info.py`; `test_patches.py` and `test_game_check.py` rewritten for CTR | |

## Kept

- `gpu/shim/bbport_gnm_hooks.cpp`, `bbport_guest_hooks.cpp`, `bbport_heap_sites.cpp`,
  `bbport_free_check.cpp`: byte-checked against the image, inert on CTR; on only with their
  BB_* switches. 2026-10-10: the libGnm function table (`gpu/shim/bbport_gnm_table.inc`, 500
  offsets with prologue bytes copied from Bloodborne's executable) and `tools/re/gnm_names.txt`
  were deleted before publication; the hook module compiles to nothing without the table, which
  `tools/re/gen_gnm_table.py` regenerates locally (git-ignored).
- The temporal upscalers, TAA, object motion vectors and scene proxy: compiled in, off by default
  (`BB_UPSCALER`, `BB_OBJECT_MOTION` override). Their settings keys stay in bbport.ini.
- `src/import_names.inc`: the compiled fallback name table (out/import_names.txt is generated
  per eboot and is what run.sh passes).
- `scripts/game_check.py` keeps the profile mechanism with one entry (ctr); `BB_GAME_PROFILE`
  is still exported for the runtime and the GPU library.
- Upstream docs under `docs/` (history, upscaler, TAA notes) describe bbport as it was.

## Interface changes

- `scripts/patches.py` has one mode (what `--external-only` did); `--external-only` is accepted
  and ignored. Defaults: `--patches-dir patches`, `--title-id CUSA13795`, `--app-version 01.21`.
- run.sh: `BB_GAME_DIR` defaults to `orig/CUSA13795`; no `../CUSA03173` fallback. `BB_FPS` only
  picks the default virtual vblank (30 → 60 Hz); the 60 FPS patch sets 480 Hz as before.
- `launcher/bb-launcher.sh`: `BB_LAUNCHER=bloodborne` is gone.
