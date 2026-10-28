# Discord game state (CUSA13795 1.21)

`runtime_state.c` samples the loaded game every five seconds into `ryunet_state.txt`.
Both launchers use `phase`, `mode`, `race`, `lap`, `laps`, and `race_seconds` when
present. The Discord start timestamp remains session elapsed time. A missing or
unsupported snapshot keeps basic presence; network peers alone do not imply a menu.

The reader starts at guest entry, including offline play. It reads its own process
with `process_vm_readv` / `ReadProcessMemory`, so an unmapped guest object does not
crash the game. It does not invoke guest code or scan the heap. Executable getter
signatures gate the fixed EU 1.21 layout; other layouts need separate verification.

Evidence from the plaintext SELF in `CUSA13795-1.21/eboot.bin` (image-relative
addresses, using `scripts/prepare.py`, `patches.boot_image`, `reflected_fields`,
`registered_vtable`, and x86-64 disassembly):

| Data | Evidence |
| --- | --- |
| Game mode | `getGameMode_VSC` at `0x10ddd50` calls `0x94efa0`, loads singleton `0x2ab3cd8`, reads `+0x144`. Enum registration values at `0x1dadfd0`: Invalid=0, Story=1, VehicleTesting=4, Arcade=8, OnlinePlay=16. |
| Race mode | `COctaneGameManager` singleton `0x2acde48`; reflected `_raceMode=0xd4c`. `EOctaneRaceModes` names at `0x274b940`, values at `0x1dadb10`: Invalid=0 through ModeR=15. |
| Local human | `CRacerManager` singleton `0x2ad4b70`, `_racers=0x10`; list count `+0xc`, pointer array `+0x20` (e.g. `0xff4d50`). `CRacer` vtable `0x26d5f78`, `_isAiControlled=0x40`, `_hasLocalAuthority=0xb0`. First local human is used for split screen. |
| Lap / finish | Reflected `CRacer._currentLap=0x4c`, `_finished=0x8c`, `_endOfRaceTime=0x6c`; lap increment at `0xff1b10`. `CRaceSettingsManager` singleton `0x2ad4e30`, `_lapsCount=0xc`. Clamp the start/finish boundary to the displayed lap range; omit laps for non-lap modes. |
| Clock handle | `getRaceClockTime_VSC` (`0x11936f0`) calls `0x1055290`. Cached handle at `0x2a92888`, pool at `0x2a92880`, invalid sentinel at `0x2a922d0`. Resolve the same index/generation checks as the getter. The result is an **entity**, not a clock component. Follow entity `+0x18` to its component list, then match `CRaceClockComponent` vtable `0x2592f98`, skipping components with flag `+0x20 & 0x10` (the getter calls `0x3dbf90`). |
| Clock fields | Reflected `_raceTimer=0x28`, `_countdownTimeLeft=0x3c`, `_raceClockData=0x48`; data clock mode `+0xc` (0 up, 1 down). Countdown getter at `0xf5cbf0` reads the float directly. |
| Count-up units | `0x1055322`: timer start `+0xc`, stop `+0x10`, running `+0x14`, stopped `+0x15`. Conversion constant at `0x1cab670` is **1/8192**, not 1/1000. `CTimer` virtual getter `0xf72dd0` uses timer type `+0x18`; game-time type 0 reads singleton `0x2ab4100`, ticks `+0x20`. |

Validation: synthetic snapshots cover unsupported images, menus, AI exclusion,
lap selection, count-up, stopped timers, countdown, finish, stale handles, and
unmapped objects. Launcher tests cover offline/online formatting, invalid fields,
future/stale timestamps, and transition back to menus. Memory-reader tests need
an environment allowing self `process_vm_readv`; socket tests need local sockets.

Remaining live verification: compare Discord against the HUD in each mode and
through loading, pause, restart, and finish. With no local racer the reader reports
**Menus / loading**; it does not yet identify individual menu pages or distinguish
loading from menus. No claim of live gameplay validation is made by the synthetic
tests. Unsupported timer types omit the time instead of using wall-clock time.

## No-build live validation, 2026-10-29

Run `/usr/bin/python3 tools/re/presence_live.py PID --samples 3` against an existing
Linux game process. This diagnostic opens `/proc/PID/mem` read-only, validates the
image getter signature, follows live objects, and uses the actual Python launcher's
`game_presence` formatter. It neither pauses/writes the process nor publishes a
Discord activity. GTK Python bindings and OS permission to read that process are
required; rebuilding the game is unnecessary.

The running packaged game was inspected at image base `0x800000000`. Live data:
Local Arcade / Time Trial, track `octane/t213_tiny_temple/t213_tiny_temple`, lap 1/3,
25.29833984375 seconds. Three samples two seconds apart held the same time.
The reflected game-clock `_gamePaused` boolean at `+0x28` was 1, explaining the
frozen timer; the corrected formatter reports `Paused · Lap 1/3 · 0:25`.

This exposed a real defect in the initial synthetic fixture: the cached handle
pointed directly to the component in the test, but to an entity in the real game.
Both the reader and fixture now follow entity → component list → clock. The
`_endOfRaceTime` field is also not float race seconds (the live raw value was
723615; the store at `0xfb1a60` copies an engine timestamp). Finished snapshots now
omit time until the final-duration source is verified, rather than displaying a
misinterpreted timestamp. Pause state uses the game's flag, not unchanged samples.

This validates live memory decoding and launcher formatting for the observed
paused Time Trial. It does **not** validate the newly edited C code in the running
old binary, Discord's final rendering, other modes, or transitions. The C fixture
was updated for entity indirection and pause; it was not recompiled in this no-build
validation pass.
