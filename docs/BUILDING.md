# Building, packaging and development

Player documentation is in the [README](../README.md). This file covers building from source,
packaging, the developer environment and running your own server. Contribution rules and the
release steps are in [CONTRIBUTING.md](../CONTRIBUTING.md).

## How it works

- **Native execution.** The eboot is converted offline (`scripts/`) into a flat memory image
  with the game's own PS4 libc, libSceFios2 and libSceNpToolkit2 linked in as native code. No
  CPU emulation: the game code runs at full speed.
- **A runtime for this game** (`src/runtime_*.c`) implements the PS4 OS functions the game calls:
  memory, threads, synchronisation, event flags, files, audio (ATRAC9 included), pad, saves,
  user service, network stubs. Imports are resolved by NID and library name; the names come
  from shadPS4's aerolib list, fetched once at the first start.
- **GPU** (`gpu/`): the vendored shadPS4 video core and shader recompiler with bbport's
  multi-threaded command processing (decode and draw recording on separate threads) and
  SacredFire's fixes: geometry-shader ring addressing (fur), 1D textures lowered to 2D (the
  sky's colour LUT), texture overlap resolution at 1080p, PM4 stream resynchronisation, a
  reliable on-disk shader cache.
- **Game patches** from [illusionyy/PS-Game-Patch](https://github.com/illusionyy/PS-Game-Patch)
  (`patches/CrashTeamRacing.xml`, authors illusion and TroyWarez) are compiled at start:
  60 FPS unlock, motion blur off, 1080p in PS4 Pro mode (the published address of that one was
  off by one byte; our copy is corrected).

## Requirements

- Linux x86-64 with a Vulkan 1.3 GPU. Tested: Steam Deck (AMD Van Gogh, Mesa RADV). Geometry
  shaders are required (fur), so macOS/MoltenVK is out; see
  [docs/sacredfire/10-other-platforms.md](../docs/sacredfire/10-other-platforms.md).
- Or Windows 10 (1903+)/11 64-bit with a Vulkan 1.3 GPU, built with MSYS2 CLANG64: see
  [packaging/windows/README.md](../packaging/windows/README.md). Tested: Windows 11, NVIDIA
  RTX 3070.
- Your decrypted dump of CUSA13795 with the 1.21 update merged in: copy the dumped update over
  the base game, replacing files (eboot.bin and sce_sys too), or use `tools/merge_update.py`
  (it also reads PS4 PKG Tool's Image0 + Sc0 layout; see
  [Which game dumps work](../README.md#which-game-dumps-work)). The launcher and `run.sh` check
  the executable image and say what is missing (`BB_SKIP_GAME_CHECK=1` skips the check).
- To build: GCC, CMake, Ninja, Python 3 (for CPUs without MOVBE, BMI1 or LZCNT,
  `pip install iced-x86`: [15-isa-rewrite.md](../docs/sacredfire/15-isa-rewrite.md)), glslang, SDL3,
  Vulkan headers and the libraries in `shell.nix` (with [Nix](https://nixos.org), `shell.nix`
  provides them). On Arch: `tools/deps.sh` fetches the header-only libraries the distribution
  lacks.

## Build and run

```bash
git clone --recursive https://github.com/T3CHNOLOG1C/SacredFire.git && cd SacredFire
bash build.sh                        # out/sacredfire and out/gpu/libbbgpu.so (~40 CPU-minutes)
BB_GAME_DIR=/path/to/CUSA13795 bash run.sh
```

`run.sh` prepares the image (`out/`), compiles the selected patches, builds if needed and starts
the game. The game folder defaults to `orig/CUSA13795`. Saves and the shader cache go to `user/`,
settings to `bbport.ini`; `BB_DATA_DIR` moves all of them. The first start compiles shaders as it
goes; later starts preload them from the cache (`BB_CLEAR_SHADER_CACHE=1` starts fresh).

Or the launcher (pick the game folder and the controller, then *Play*):

```bash
bash launcher/bb-launcher.sh         # launcher/install-desktop.sh adds it to the app menu
```

The [shared native launcher](../docs/shared-launcher.md) (Dear ImGui/SDL3) has pages for the game
folder, the controller, the game patches, PS4 Pro mode, display (fullscreen, FPS counter) and
developer switches (frame statistics, log to
a file, clear the shader cache, Vulkan validation, extra environment), and shows the game's
output. `--play` starts the game with the saved settings and no window (Steam Deck game mode).
The launcher uses the bundled SacredFire icon. Desktop and Steam shortcuts keep a persistent
copy in your icon theme, including when launched from an AppImage.

**Native LZMA decoder (Linux).** Recognized CTR 01.21 executables use liblzma for independent
archive blocks, keeping the original compressed game files and requiring no disk cache. The
patch checks the decoder and its one-shot caller before installing; unsupported layouts and
blocks retain the original decoder. Set `BB_NATIVE_LZMA=0` in the launcher's extra environment
to compare or disable it. The streaming queue keeps the game's four workers to limit concurrent
memory use. `BB_LZMA_WORKERS=6` experimentally enables six on hosts with at least eight logical
CPUs. Source builds require liblzma 5.4 or newer (`xz` in `shell.nix`).

## Building on Windows

1. Install [MSYS2](https://www.msys2.org) to `C:\msys64` and the packages listed in
   [packaging/windows/README.md](../packaging/windows/README.md).
2. In the **MSYS2 CLANG64** shell: `git clone --recursive <this repository>`, then
   `bash build.sh` (builds `out/sacredfire.exe`).
3. Merge the 1.21 update over the base game without copying it:
   `python tools/merge_update.py <CUSA13795> <CUSA13795-patch> <CUSA13795-1.21>`.
4. Run: `set BB_GAME_DIR=<CUSA13795-1.21>` and `C:\msys64\clang64\bin\python.exe run.py`.
   In the game, **Insert** (or L3+R3) opens the port's menu.

For local builds, GitHub Actions builds, and tagged releases, follow the
[desktop build guide](../docs/MANUAL_BUILDS.md).

CPUs without MOVBE, BMI1 or LZCNT (Sandy Bridge, Ivy Bridge) also need `iced-x86` for the Python
that runs `run.py`, which the port uses to rewrite those instructions at start
([15-isa-rewrite.md](../docs/sacredfire/15-isa-rewrite.md), issue #31). MSYS2's Python cannot
install the wheel: use a python.org Python 3 (`pip install iced-x86`) or the portable ZIP from
Releases, which bundles it.

## AppImage (Steam Deck)

`packaging/appimage.sh` builds `dist/SacredFire-x86_64.AppImage` with Nix: the
binaries are built in `nix-shell shell.nix` so the bundle can collect their libraries, then
`nix bundle` packs them with the native launcher, Python and Mesa's Vulkan drivers (the header of
the script has the exact steps). Running it opens the launcher; `--play` starts the game. Data
lives in `~/.local/share/sacredfire`, including `launcher.ini`. The old GTK settings in
`~/.config/sacredfire-launcher/settings.json` are imported once and preserved.
Without a game folder chosen, the launcher shows the Game row as *not chosen* with Play disabled
until a folder passes the game check.

The [desktop build guide](../docs/MANUAL_BUILDS.md) has the GitHub Actions workflow, Nix
commands, version checks, Windows ZIP steps, and output checks. Maintainers: follow the
[release instructions for Ryu](../docs/shared-launcher.md#release-instructions-for-ryu) for
every AppImage release.

## Environment variables

### Paths and launcher

- `BB_GAME_DIR`: the game folder (default `orig/CUSA13795`).
- `BB_USER_DIR`, `BB_DATA_DIR`: saves, shader cache and settings.
- `BB_CONFIG`: the launcher's `bbport.ini`.
- `BB_PATCHES_DIR`: the patch folder.
- `BB_SKIP_GAME_CHECK=1`: skip the executable check.

### Graphics and performance

- `BB_NEO=1`: PS4 Pro mode.
- `BB_ANISO=N`: anisotropic filtering, 16 by default.
- `BB_PC_MODEL=1`: bbport's experimental in-place memory model; it frees CPU time but needs more
  GPU-visible memory than the Steam Deck has.
- `BB_FULLSCREEN=1`, `BB_PRESENT_MODE`: fullscreen and presentation mode.
- `BB_CLEAR_SHADER_CACHE=1`: start with an empty shader cache.
- `BB_VK_VALIDATION=1`: Vulkan validation layer.
- `BB_BREADCRUMBS=0`: no GPU breadcrumbs.
- `BB_GPU_PROFILE=1`: GPU profile output.

### Input

- `BB_GAMEPAD`: the gamepad for port 1, by GUID or part of its name.
- `BB_PAD_RECORD=file` and `BB_PAD_REPLAY=file`: record and replay input.

### Timing and diagnostics

- `BB_FPS=30|60`: virtual vblank; the 60 FPS patch sets 480 Hz by itself.
- `BB_FRAME_STATS=1`: frame statistics in the log, including VRAM, GTT and RSS.
- `BB_TIMEOUT=N`: stop after N seconds with thread stacks; 0 never stops.
- `BB_PRESENT_DUMP_TRIGGER=file` with `BB_PRESENT_DUMP_COUNT=N`: dump presented frames.
- `BB_CAPTURE_TRIGGER=file`: frame analysis.

### Off by default

bbport's temporal upscalers (`BB_UPSCALER`, `BB_OBJECT_MOTION`, and `BB_UPSCALER_MENU=1` for
their menu sections) were built for Bloodborne's scene and stay off.

## Running your own RyuNET server

The game only accepts auth signed by a key it trusts, so a server that does not use the
production key needs that key on the client: put the server's `ctr_ps4.pub` in the data folder
as `ryunet_keys/<host>.pub` (for a local checkout, `ryunet_keys/127.0.0.1.pub`), or set
`BB_AUTH_KEY=<path>`. Set the server in the launcher's Server field or with `BB_ONLINE_SERVER`.
Switching back needs nothing: each launch removes the key patch when the run does not need it, so
the production key always applies again. A server that signs with the production key needs no
patch at all. A server whose key is not at hand prints a warning: without that key the game
retries auth for ever, with nothing in its own log saying why.

`python scripts/dev.py` runs the game against `127.0.0.1` and removes the key patch on exit,
Ctrl-C included. Set `RYUNET_DIR` to your server's folder to have it started too (its
`bin/server`); with a server already running the script only launches the game. The address is
passed to the run only, never saved, so the packaged game and its server setting are untouched.
Use `BB_NO_SERVER=1` to never start one, and `BB_DATA_DIR` as usual for another data folder.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | Loader (`probe.c`) and the runtime |
| `scripts/` | Game image preparation, module linking, game check, patch compiler, mods |
| `gpu/` | Renderer: vendored shadPS4 video core, shims, ImGui menu |
| `patches/` | Community patches (`CrashTeamRacing.xml`) |
| `launcher/`, `packaging/` | Native launcher, desktop entry, Nix package, AppImage |
| `tools/` | Import names, scripted runs, screenshots, A/B toggles, GPU checks |
| `tests/` | Loader, runtime, patch, game-check and renderer tests |
| `docs/` | Port and renderer notes: [status](sacredfire/STATUS.md), [upscaler](upscaler.md), [parallel GPU](parallel_gpu.md), [motion vectors](motion_vectors.md), [roadmap](ROADMAP.md), [bbport notes](bbport/README.md) |

Tests:

```bash
bash build.sh --test
python3 -m unittest discover -s tests
ninja -C out/gpu motion-history-test ui-composition-test scene-resolution-test \
  motion-shader-test settings-test
```

## History

SacredFire started from bbport on 2026-10-09. The Bloodborne profile, its patches, launcher and
FSR 4 tooling were removed the same day
([docs/sacredfire/09-bloodborne-strip.md](../docs/sacredfire/09-bloodborne-strip.md)). bbport's
temporal upscalers and motion vectors remain in `gpu/`, off; its libGnm hook code remains without
a function table (the table was generated from Bloodborne's executable and is not distributed).
