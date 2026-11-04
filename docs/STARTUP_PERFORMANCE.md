# Startup preparation performance

Status: measured on Linux with USA CUSA13795 01.21, 2026-11-04. Native
Windows, EU timings, cold-storage runs, and time to the first game frame remain unmeasured. I also launched the EU edition on Linux and played a race.
Automated checks were subsequently run before PR submission: the Linux source-only suite passed (445 tests, 74 skips). Native loader fixtures have nine reproducible pre-existing failures; see BOOT_CACHE_PROGRESS.md. The GPU library was not rebuilt.

## Startup path and plan

Both launchers prepare the following dependencies before starting the runtime:

1. Online configuration, game validation, region/version selection, mod session.
2. Graphics-tab archive and mod file redirections.
3. Boot cache validation or generation, import names, DLC profile, shader extraction.
4. Playlist, controller prompts, aspect-ratio overlays, mod overlays, item rules.
5. Time Trial references, Online menu/package assets, Itemless overlays.
6. Executable patches, render configuration, CPU/GPU capability selection.
7. Native runtime: image loading, graphics initialization, shader/pipeline warmup,
   guest execution and first frame.

The delay between `Time Trial references: up to date` and `ITEMLESS: private
overlays prepared for 40 tracks` includes **both Online Mode and Itemless**.
It is not evidence of a runtime hang or of shader compilation.

The plan is to measure preparation separately, remove redundant copies and
repeated CPU work, preserve validation and output bytes, and expose per-stage
timings for actual launches. Runtime/GPU changes require their own measurements;
preparation timings do not measure time to the first frame.

## Measurements and changes

Python 3.15 on the local Linux host; existing USA game files read directly.
Measurements used cProfile and perf_counter, with generated output confined to
`out/startup-profile`. The OS file cache was not flushed. Initial and final
Online/Itemless measurements used an empty base-overlay list, not the complete
my full mod/server configuration. These are preparation microbenchmarks.

| Stage | Initial | After changes | Evidence |
| --- | ---: | ---: | --- |
| Itemless | 3.825 s | 0.833 / 0.871 / 0.881 s | Three final runs; 8 generated overlays byte-identical |
| Online Mode | 1.457 s | 1.097 / 1.096 / 1.001 s | Three final runs; 27 generated PAK/IGZ/overlay files byte-identical |
| Game validation/profile | 0.363 s | Unchanged | Loaded-image hashing and live archive checks |
| Boot input hashes | 0.073 s | Unchanged | Full input digests retained |
| Boot output verification | 0.345 s | Unchanged | Full cached-output digests retained |
| DLC profile | 0.009 s | Unchanged | Existing DLC directory, isolated output |
| Playlist | 0.326 s cold; 0.007 s warm | Existing cache retained | No new persistent cache |
| Controller prompts | 2.680 s cold; <0.001 s warm | Existing cache retained | Xbox prompts, local configuration |
| Aspect ratio | <0.001 s | Unchanged | Local configuration; not a widescreen-build benchmark |
| Executable patches | 0.101 s | Unchanged | Isolated output, existing patch configuration; subprocess startup included |

Itemless originally copied large nested track PAKs into temporary files every
launch. A bounded read-only memoryview now accesses the parent's mmap directly.
Only selected assets are extracted; child offsets remain relative to the child,
and the child view is released on success and exceptions. This removes the
temporary PAK writes and their writable-directory requirement on Windows/Linux.
Crate processing also reuses object/string-reference lookups and the asset
override set instead of rebuilding them inside loops.

Online package generation repeatedly sorted the complete IGZ reference list after
each insertion. Binary-search insertion preserves its sorted, duplicate-free
contract with less work, following the approach already used by racer_registry.

The initial optimization did not introduce a persistent asset cache. The later
upstream integration retains Ryu's Itemless cache; its source identity now also
includes the canonical game directory and resolved mod archive identity, size,
mtime and ctime. This uses metadata rather than full archive hashing, following
upstream's invalidation model. Normal game/mod updates invalidate the cache;
arbitrary content tampering that preserves every recorded identity/metadata field
is not detected by this asset cache.
The startup audit also corrected import-name generation to read `link.json` from
the selected boot cache, rather than stale legacy output.

Raw evidence: `out/startup-profile/{online,itemless}.txt`, corresponding
`*-final.txt`, `results.json`, and `full/*.txt`. These local artifacts are not
committed and contain no bundled game data in the documentation.

## Launch tracing

Set `BB_STARTUP_TIMING=1` for either launcher. Shell preparation timings go to
stderr so captured `key=value` and path output remain valid. The Python launcher
uses its normal logger for subprocess and direct preparation calls. Timings are
disabled by default and report failures too.

Example for the existing Linux runtime and my game/data directories:

```bash
BB_STARTUP_TIMING=1 BB_PREBUILT=1 \
BB_PROBE="$PWD/out/sacredfire" \
BB_GAME_DIR="$HOME/Games/CTR/CUSA13795-1.21" \
BB_DATA_DIR="$HOME/.local/share/sacredfire" bash run.sh
```

On Windows, set `$env:BB_STARTUP_TIMING='1'` before invoking the existing Python
launcher. No GPU rebuild is required for these Python/shell changes.

## Progress and remaining acceptance

### Online connection optimization

I profiled the network stage after measuring 2.520–2.594 s during my launches.
A public GET took 1.181 s, with about 0.814 s spent establishing the connection
(0.447 s in the TLS handshake). The response itself did not account for most of
that delay.

`scripts/ryunet_session.py` now keeps connections only during one sequential
online setup. Settings are fetched first, the server clock is applied, and nickname
requests follow on the same connection when the server permits it. Responses are
not cached. TLS certificate/hostname verification remains enabled. Configured
proxies use the existing urllib opener; redirects/errors still flow through the
existing RyuNET fallback rules with fresh signatures for each attempt. The session
does not replay failed POSTs, and discards failed, partially consumed or
server-closed connections. All connections close when setup finishes.

The address lookup for the IP fallback is also lazy: a successful named endpoint
does not need a separate `gethostbyname` call first.

I measured two GETs to the public settings endpoint using the integrated request
code, without a nickname or PC fingerprint:

| Connection mode | First GET | Second GET | Total |
| --- | ---: | ---: | ---: |
| Separate connections | 1.102 s | 1.234 s | 2.336 s |
| Reused connection | 1.170 s | 0.458 s | 1.628 s |

The measured saving is 0.708 s for this pair. Network conditions were not
controlled, and this does not establish the authenticated launch's speedup.
Evidence: `out/startup-profile/network/integrated.json` and `urllib.txt`.
Linux regression tests now pass, including proxy delegation and local HTTP fixtures; native Windows behavior still needs verification. Python changes
take effect on the next launch without rebuilding the runtime or GPU.

I subsequently tested a real Linux USA launch (`20261104_082314.log`). Online
configuration took 1.647 s, including settings at 1.177 s and nickname checking at
0.402 s, versus 2.594/1.295/1.230 s in my earlier 08:16 run. Total measured
preparation fell from 4.900 to 3.785 s. Item rules 0.2 were loaded and the game
continued rendering. These are two launches under uncontrolled network/storage
conditions; they do not establish a guaranteed speedup or first-frame latency.

### My launch results, 2026-11-04

I tested the Linux USA build v0.7.5-48-g9c52580-dirty and recorded its launch output.
Boot cache explicitly reported a hit. The printed preparation durations sum to
5.250 s (not an end-to-end first-frame measurement). The largest stages were
online_config 2.637 s, online_mode 0.677 s, boot_cache 0.500 s, game_check 0.258 s
and mod_launch 0.226 s. speed_mode took 0.110 s including interpreter startup and
my actual base-overlay inputs; this is not comparable directly to the
7 ms in-process empty-overlay microbenchmark. The interval's Online/Itemless
preparation totals 0.787 s.

The log reached Vulkan instance/device initialization but ended during capability
warnings; it contains neither a first-frame time nor evidence of successful game
entry. The network stage combines fresh server settings and nickname checks,
whose separate durations were not logged. Optional timing now reports
online.server_settings and online.claim_nick to distinguish those requests on the
next launch. These are nested timings inside online_config, not additional times
to add to its total. No requests, test suites or builds were run for this follow-up.

### Integration with upstream 9c52580

Follow-up saved launch evidence: `20261104_081516.log` shows 4.544 s of top-level
preparation, including online_config 2.520 s (server settings 1.299 s, nickname
check 1.138 s), boot-cache hit 0.279 s, Online Mode 0.556 s and Itemless 0.100 s.
The game entered guest code and produced a frame CSV before I closed the
window. The logged `Startup: -u 9.991 s` was the runtime log-forwarding lifetime,
not startup latency; that timing wrapper and the post-exit-report wrapper have
been removed. First-frame latency still cannot be inferred from this log.

On 2026-11-04, main was fast-forwarded from 2890858 to 9c52580 (five commits).
Local edits reapplied without Git conflicts. The pre-integration stash and
`out/before-main-update-20261104-080619/` backup were retained. Existing FSR
submodule changes were preserved.

Upstream adds Itemless caching and disables Steam Fossilize recording for shortcut
launches. Both remain active. Local copy/lookup optimizations affect cache builds;
cache hits skip those operations. Boot caching remains a separate preparation
stage, not a second Itemless cache.

A separate, non-cProfile comparison used upstream speed_mode/archive_reader/igz_file
source versus the integrated versions, the same USA game, empty base overlays,
isolated outputs, two cache builds and five hits each:

| Itemless operation | Upstream | Integrated |
| --- | ---: | ---: |
| Cache build | 5.163 s; 52.390 s | 0.427 s; 0.323 s |
| Cache hit | 7.13–7.47 ms | 6.66–7.23 ms |

The 52-second upstream build is a large outlier during repeated temporary PAK
writes; storage conditions were not controlled, so it is not used as a claimed
speedup factor. Cache-hit costs are comparable at roughly 7 ms, and all eight
output overlays matched byte-for-byte. Resolving every archive path redundantly
initially cost 22–39 ms per hit; the final version resolves the game root once and
uses source file identity/stat data, avoiding that regression.

Evidence: `out/startup-profile/upstream-comparison/`, including source snapshot,
measurement runner, JSON results and isolated overlay outputs. Python AST parsing,
shell syntax inspection and diff whitespace checks completed after integration;
no test suite or build was run during integration. I subsequently prepared a
runtime-only build, which produced `v0.7.5-48-g9c52580-dirty` (log:
`out/cpu-build-163.laAs9N/build.log`). Native runtime changes are now included;
the reused GPU library still predates the upstream GPU changes. No game session
or test suite was started.

- Complete: static launcher/dependency trace, profiling of Online/Itemless and
  additional local preparation stages, measured copy/CPU improvements, output
  comparisons, optional per-stage logging, import-path correction.
- Passed on Linux: regression fixtures for nested bounds/view cleanup and
  ordered IGZ insertions in `tests/test_startup_preparation.py`.
- Pending: actual launch with my complete overlay/mod chain, end-to-end
  first-frame timings, native Windows runs, automated region switching, malformed archive fixtures,
  native Windows permission/path tests and actual package validation.
- Network setup, shader extraction/warmup, enabled mods and native runtime stages
  have not been benchmarked in this pass. No end-to-end speedup or zero-bug claim
  is made from preparation microbenchmarks.
