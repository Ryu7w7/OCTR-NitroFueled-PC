# SacredFire on macOS (Apple silicon) — experimental

The game's code is x86-64 and runs as it is, so the port is an x86-64 process under Rosetta 2.
What Rosetta allows was measured first (`tools/macos-smoke`,
[docs/sacredfire/19-macos-apple-silicon.md](../../docs/sacredfire/19-macos-apple-silicon.md)).
**Status (2026-10-22):** builds and plays on an M1 (8 GB, macOS 26.2): menus, races, audio,
controller, 27-30 FPS at 1080p. Fur and hair are drawn although MoltenVK has no geometry shaders:
those draws are emulated with vertex shaders (`vk_gs_emulation.cpp`,
[19-macos-apple-silicon.md](../../docs/sacredfire/19-macos-apple-silicon.md)), except during the
Halloween season event (fur skipped then: it crashed MoltenVK). Linux-only
diagnostics (BB_FREE_CHECK, BB_COPY_PROTECT, BB_SAMPLE_THREAD, the PC memory model,
userfaultfd) are off, as on Windows.

## Requirements

- A Mac with Apple silicon (M2 or newer tested target), macOS 15 or newer.
- Rosetta 2: `softwareupdate --install-rosetta`
- Xcode command line tools: `xcode-select --install`
- Homebrew (the usual arm64 one, [brew.sh](https://brew.sh)): `build.sh` installs `cmake`,
  `ninja` and `pkg-config` with it; these build tools run natively. (Homebrew no longer installs
  x86-64 packages, so the libraries do not come from it.)
- The **LunarG Vulkan SDK** for macOS ([vulkan.lunarg.com](https://vulkan.lunarg.com/sdk/home#mac)):
  the Vulkan loader, MoltenVK and glslang as universal (arm64 + x86-64) binaries. The default
  install location (`~/VulkanSDK/<version>/macOS`) is found by itself; otherwise set `VULKAN_SDK`.
- The x86-64 libraries (SDL3, FFmpeg, Boost, fmt, Zydis, xxHash, ...) are built from source by
  [vcpkg](https://vcpkg.io) (triplet `x64-osx`, list in `vcpkg.json`). `build.sh` clones and
  bootstraps it into `out/vcpkg` on the first build, unless `VCPKG_ROOT` names one; that first
  configure takes a while (FFmpeg) and logs to `out/gpu-configure.log`.
- Your own decrypted dump of CUSA13795 with the 1.21 update merged
  (`python3 tools/merge_update.py <base> <patch> <out>`).

## Build and play

```
git clone --recursive <this repository> && cd <it>
BB_GAME_DIR=/path/to/CUSA13795-1.21 bash packaging/macos/play.sh
```

`play.sh` builds on the first start (`build.sh`, Darwin branch: CMake + Ninja for x86-64) and
then starts the game through `run.sh`. Settings live in `bbport.ini` (in-game menu: L3+R3).
The first start after an update recompiles the game's shaders: a few minutes on the title screen.

## The app (DMG)

```
bash packaging/macos/make-dmg.sh
```

makes `dist/SacredFire-macOS.dmg` (about 30 MB) from the current build: `SacredFire.app` with the
x86-64 game binaries, the scripts, the launcher, and the Vulkan loader and MoltenVK from the SDK
(run as `BB_PREBUILT`, like the Linux AppImage). Players need only Rosetta 2 and Homebrew: on its
first start the app installs the launcher's GTK 4, libadwaita, Python and Bash 4 with Homebrew in a
Terminal window. Data: `~/Library/Application Support/SacredFire`. The app is signed ad hoc, not
notarized: right-click → Open the first time. A release must match a published commit (GPL), as
for the AppImage.

## The launcher

```
bash packaging/macos/make-app.sh
```

makes `~/Applications/SacredFire.app` (Launchpad, Spotlight, the Dock): the same launcher as on
Linux (`launcher/sacredfire_launcher.py`, GTK 4 + libadwaita from Homebrew, installed by the script
when missing; it runs natively). Choose the game folder there, set graphics (internal resolution,
FPS limit, patches), controls, saves and online play, and press Play: it starts
`packaging/macos/play.sh` with those settings. Its log: `~/Library/Logs/SacredFire.log`. The app
runs the launcher of this folder: after moving the folder, run the script again. The Steam page is
hidden on macOS.

## How the port differs from Linux

| Linux | macOS |
|---|---|
| Guest TLS: GS base set with `arch_prctl` | GS is the pthread TSD array: the loader rewrites the guest's `mov rax, gs:[0]` to `gs:[8*key]` of a pthread key (as Windows does with a TEB slot) |
| Guest user range from 0x1000000000 | From 0x7000000000: Rosetta reserves 0xfc0000000..0x7000000000. The whole range is claimed once (`runtime_memory_reserve`) and unmapping puts the placeholder back |
| memfd + hole punching | An unlinked POSIX shm object; released memory is cleared |
| The window on its own thread | AppKit needs the main thread: the game runs on a second thread and the main thread runs the window (`bbgpu_main_thread_loop`) |
| CPU check with cpuid | Rosetta hides AVX/F16C/BMI1/LZCNT/MOVBE from cpuid but runs them: the check is skipped under Rosetta |
| Vulkan driver | MoltenVK through the loader, with portability enumeration |
