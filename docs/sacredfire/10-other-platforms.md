# Windows and macOS: what a port would need (notes, 2026-10-09)

> 2026-10-12: Windows now builds and runs natively, with the Windows layer of Supermedo's bbport
> port merged in; see [packaging/windows/README.md](../../packaging/windows/README.md). The notes
> below are the analysis from before.

SacredFire inherits bbport's design: the game's x86-64 code runs natively, a small C runtime
(src/) stands in for the PS4 system libraries, and the GPU library (gpu/) translates the PS4
command stream to Vulkan. Nothing in that design is Linux-only in principle, but the
implementation is, in these places.

## Loader and runtime (src/)

| mechanism | today (Linux) | Windows | macOS |
|---|---|---|---|
| Image at 0x800000000, direct memory, flexible memory | mmap with MAP_FIXED, mremap, memfd (probe.c, runtime_memory.c) | VirtualAlloc/MapViewOfFile at fixed addresses; no mremap | mmap works on Intel; Apple Silicon has no native x86-64 |
| Guest TLS (the game reads %gs / %fs) | arch_prctl(ARCH_SET_GS) per thread (runtime_thread.c); libc.prx's fs accesses rewritten by link_libc.py | gs holds the TEB: every guest TLS access would need another base register or a wrpkru-free trick; shadPS4 patches TLS instructions on the fly | gs is Apple's own TLS base: the same problem |
| Guest faults, watchdog, write tracking | sigaction/SIGSEGV handlers, userfaultfd (BB_UFFD), mprotect | vectored exception handlers; no userfaultfd (page-guard tracking instead) | Mach exceptions or BSD signals; no userfaultfd |
| Host-implemented imports (~730) | called directly by the game with the SysV ABI (`ABI` in runtime.h is `sysv_abi`) | Windows x64 has a different calling convention: each exported function needs a thunk or `__attribute__((sysv_abi))` on a compiler that supports it (GCC/Clang do) | SysV already |
| Threads, sync, time | pthreads, futex-backed event flags, CLOCK_MONOTONIC | Win32 threads or winpthreads; condvar clocks differ | pthreads, fine |
| Files, saves | POSIX, getdents, ftw | path and case rules differ; no getdents | POSIX, fine |
| Audio | SDL3 (runtime_audio.c), LibAtrac9 | SDL3 works | SDL3 works |
| Controllers | SDL3 gamepads, re-enumeration loop for the container | SDL3 works (no udev issue) | SDL3 works |

## GPU library (gpu/)

- The vendored shadPS4 video core builds on Windows and macOS upstream; bbport's shims add
  Linux pieces: dma-buf / memfd guest memory (bbport_guest_memory.cpp, the PC memory model),
  write-tracking hooks, and the Linux build system (build.sh, CMake with GCC and LTO).
- Windows: native Vulkan drivers, so the renderer itself is the easy part.
- macOS: Vulkan only through MoltenVK, which has no geometry shaders. CTR's fur and hair go
  through a geometry-shader ring (06-fur.md), so those characters would not render; shadPS4 on
  macOS has the same gap. Apple Silicon adds x86-64 translation (Rosetta 2 supports AVX2 only
  since macOS 15, and fixed-address mappings at 0x800000000 are not guaranteed), so the realistic
  macOS target is an Intel Mac, a deprecated platform.

## Build and packaging

- Scripts are Python 3 and Bash (prepare, link, patches, game check): portable with Bash on
  Windows (Git Bash/MSYS2) or a small rewrite of run.sh.
- The AppImage is Linux-only; Windows would want a zip or installer, macOS a .app bundle.

## What is realistic

- **Windows, properly**: a new loader and memory layer, the TLS scheme, exception handling
  and the ABI thunks. Weeks of work; shadPS4's Windows code shows every trick needed. Only
  the runtime's game-facing logic (imports, event flags, saves, pad) carries over as is.
- **Windows, pragmatically**: run the Linux AppImage in WSL2 with WSLg. GPU access goes
  through the Mesa d3d12 (Dozen) Vulkan driver, which is incomplete and slow; expect missing
  features and a fraction of native performance. A Linux VM with GPU passthrough is the other
  route.
- **macOS**: not realistic for Apple Silicon; an Intel Mac running Linux can use the AppImage.
