# Replay Mode in the spectator build (experimental)

`ReplayMode.xml` is Erluinzo's [Replay Mode](https://github.com/Erluinzo/ctrnf-replay-mode) 0.2.2
(MIT, 444e627), USA CUSA13795 1.21, rebuilt with `octr-mod-code-area.patch`:

- its code goes in the mod code area (0x2340000-0x2380000) that this branch maps after the game's
  code segment (`scripts/patches.py` `code_extension`), instead of the segment's last padding,
  where gcc 13-15 builds no longer fit;
- `memcmp` added to `src/mem.c` (gcc 15 calls it).

Build: clone the mod next to a folder `CTRNF` holding `out/eboot.elf` (USA 1.21), apply the patch,
`python3 build.py` (gcc 15 + binutils, WSL). The EU build needs `CTRNF/orig/CUSA14876-eboot.elf`.
