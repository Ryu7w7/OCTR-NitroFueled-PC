# Setup notes

- Sessions run in a podman/distrobox container `archbuild` (Arch) on the Steam Deck with
  passwordless sudo; `sudo pacman -S --needed boost robin-map miniz zydis spirv-cross` provided the
  repo dependencies; `tools/deps.sh` fetches magic_enum, VulkanMemoryAllocator and xbyak
  (header-only) into out/deps and build.sh adds the prefix to CMAKE_PREFIX_PATH.
- The untouched bbport 0.4 tree built on the first try once the dependencies were present
  (gcc 16, cmake 4.4; ~40 CPU-minutes for libbbgpu.so). `out/sacredfire --vulkan-only` passes on
  RADV VANGOGH.
- The file layer already refuses every write under /app0 and /hostapp with EROFS
  (`game_path()` in src/runtime_file.c: open with write flags, mkdir, rmdir, unlink, rename,
  truncate). `find orig -newer plan.md` is empty after every run.
