# Boot cache progress

Last updated: 2026-11-04. Overall status: draft implementation, tests, and CI configuration written; authorized Linux runtime-only build completed. I manually verified Linux USA cache-hit launches and played a race with EU on Linux; Linux cache contracts and the source-only suite pass; native Windows validation remains pending. Not ready to merge as the fix for issue 163.

I use RTK and keep the documentation in English. I test with the rebuilt runtime, existing GPU library, and existing game/data directories. Automated testing is now enabled; GPU rebuilds and package builds remain deferred.

References: [issue 163](https://github.com/Ryu7w7/OCTR-NitroFueled-PC/issues/163), [design](BOOT_CACHE_DESIGN.md), [plan](BOOT_CACHE_PLAN.md).

## Work stages

| Stage | Status | Evidence or remaining work |
| --- | --- | --- |
| Issue review and initial audit | Complete | Repeated writes found in run.py/run.sh and three preparation scripts; baseline 2890858bc583b2126d9889889f039dec10d56244 |
| Design and plan documentation | Complete | BOOT_CACHE_DESIGN.md and BOOT_CACHE_PLAN.md |
| Full audit and runtime baseline | Startup profiles recorded; first-frame baseline pending | Runtime and diagnostic consumers traced; close_range found in Linux restart |
| Cache implementation and contract tests | Linux passed; Windows pending | boot_cache.py, launch_lock.py, test_boot_cache.py, test_boot_cache_paths.py |
| Launch and consumer integration | Linux USA launch checked; remaining configurations pending | run.py/run.sh, patches, widescreen, online, ISA, and got_import use explicit boot inputs |
| Native CI and packaging | Configuration written; not executed | Ubuntu/Windows job added; source inspection shows packaging copies scripts automatically |
| Linux runtime-only build | Completed | out/sacredfire linked against the existing out/gpu/libbbgpu.so; GPU SHA-256 unchanged |
| Real-game tests and benchmarks | Partial evidence | My launch log confirms a USA boot-cache hit; Linux preparation profiles recorded in STARTUP_PERFORMANCE.md; EU racing also checked on Linux; Linux contracts passed, native Windows validation pending |
| Review and merge acceptance | Static review performed; not ready to merge | Behavior and performance evidence remain outstanding |

## Outstanding decisions and verification

- Verify fingerprint dependency coverage and runtime format compatibility through tests.
- Verify separation of boot input directories from dynamic output for every consumer.
- Verify locking, restarts, and retention natively on Windows and Linux.
- Identify available USA/EU builds and native validation environments.

Resolve these through the planned audit and testing. Automated testing is now enabled; GPU rebuilds and package builds remain deferred.

## Runtime evidence matrix

| Environment | USA | EU | Cache hits and updates | Recovery and concurrency |
| --- | --- | --- | --- | --- |
| Native Windows and portable ZIP | Not tested | Not tested | Not tested | Not tested |
| Native Linux checkout | I tested USA 01.21 startup and rendering | I launched EU and played a race | Cache hits observed; update/invalidation tests pending | Not tested |
| Linux AppImage | Not tested | Not tested | Not tested | Not tested |

## Progress recording rules

Mark work complete only when reviewable evidence exists. For each test, record the commit, OS/Python version, game build or fixture, command/steps, result, and log location. Distinguish fixture tests, process/filesystem tests, and real-game tests. Record skipped tests with reasons rather than marking them passed.

For benchmarks, record machine/storage conditions, repetitions, preparation time, time until the game appears, bytes read/written, and cache size. Never report estimates as measurements.

## Automated validation before PR submission

I ran the checks on Linux with Python 3.14.7 and iced-x86 1.22.0 in an isolated virtual environment.

| Check | Result |
| --- | --- |
| Boot-cache/path contracts | 22 tests: 21 passed; Windows extended-length path case skipped |
| RyuNET requests/session behavior | 11 passed using local HTTP fixtures |
| Full source-only suite, matching CI without prebuilt executables | 445 tests: 371 passed, 74 skipped |
| Python compilation, shell syntax, patch XML, diff whitespace | Passed |
| Native loader fixtures with local prebuilt | 15 tests: 6 passed, 9 failed; same failures reproduced with the pre-change runtime |

I fixed the generated boot fixture's duplicate module identities and updated shell-invocation fixtures to include the actual timing wrapper and selected boot directory. The native loader failures stop on `native race option callback could not be installed` for small synthetic images; these also occur with the saved pre-change binary and remain outside this startup-only change.

The full source-only run used a separate snapshot without `out/` binaries, as in CI. Skips cover Windows-only cases and tests requiring optional game dumps/native executables; they are not counted as passes. Logs: `out/pr-source-checks.log`, `out/pr-probe-baseline.log`, and `out/pr-launch-integration.log`.

Reproduce the source suite in a clean checkout after installing iced-x86:

```bash
python3 -m unittest discover -s tests -p 'test_*.py' -v
```

## Work history

### EU manual validation

- I launched the EU edition on Linux and played a race. This confirms manual EU gameplay in addition to the logged USA launches; it does not establish native Windows, automatic region-switching or failure-recovery coverage.
- EU timing numbers and the exact game version were not recorded in the benchmark table.


### My launch after connection reuse, 2026-11-04 08:23

- I tested the integrated startup changes and recorded `20261104_082314.log`. Top-level preparation totals 3.785 s, compared with 4.900 s in my 08:16 run; this is a single-run comparison, not a controlled benchmark.
- Online configuration fell from 2.594 to 1.647 s. Nickname checking fell from 1.230 to 0.402 s; settings retrieval took 1.177 s. Server item rules 0.2 remained present.
- Boot cache reported a hit (0.327 s), Online Mode took 0.642 s and Itemless 0.083 s. The game entered guest code and continued rendering while I reviewed the growing log.
- Runtime HTTP responses included 200 for the authentication-token request. A later 403 belonged to the redirected race-start telemetry endpoint, not the startup nickname/settings calls. No fatal-error or traceback marker appeared in the reviewed snapshot.
- Native Windows, region-switching and proxy/failure-path regression suites and exact launch-to-first-frame timing remain outstanding. No build or test suite was run during this log review.

### Online connection reuse, 2026-11-04

- I profiled public settings requests after my launch logs showed about 2.6 s in online configuration. The initial profile attributed about 0.814 s to connection establishment, including 0.447 s in the TLS handshake.
- I added a short-lived HTTP session shared by the sequential settings and nickname requests. Fresh responses, certificate/hostname verification, server-clock synchronization and per-attempt signatures remain in place. Connections close after setup; proxies retain the existing urllib route.
- I deferred the explicit fallback-address DNS lookup until both named endpoints need the filter-bypass path. Successful HTTPS no longer performs that extra lookup.
- I measured two public GETs with no nickname or machine fingerprint: separate connections took 1.102 + 1.234 = 2.336 s; reuse took 1.170 + 0.458 = 1.628 s. This is a network microbenchmark, not a full authenticated launch result.
- I recorded results in `out/startup-profile/network/integrated.json` and the initial profile in `urllib.txt`. No nickname registration or server mutation was performed.
- I added regression coverage for connection reuse, fresh settings, filter fallback/signatures, lazy DNS, proxies, incomplete responses and failed POST handling. Tests remain unexecuted; Python syntax and whitespace checks pass. No rebuild is needed for these Python changes.

### My repeat launch, 2026-11-04 08:16

- I reviewed `20261104_081625.log` against `20261104_081516.log`. Top-level preparation totals 4.900 s versus 4.544 s; no end-to-end improvement is claimed from these two runs.
- Online configuration remains largest at 2.594 s (settings 1.295 s, nickname 1.230 s). Boot-cache hit took 0.382 s, Online Mode 0.652 s, Itemless 0.101 s.
- Frame CSV contains 960 records spanning 5.5826 s from its first recorded frame. Runtime entered guest code and ended on a window-close request; no fatal-error/traceback marker was found in this log review.
- The misleading runtime-lifetime startup timing is absent following the instrumentation correction. No tests or builds run for this review.

### My saved launch log review, 2026-11-04 08:15

- I reviewed `~/.local/share/sacredfire/logs/20261104_081516.log` and its frame CSV. Runtime entered guest code, rendered frames and logged a window-close request at shutdown.
- Top-level preparation durations total 4.544 s, excluding nested online timings. Fresh server settings took 1.299 s and nickname checking 1.138 s, within online_config's 2.520 s.
- Boot-cache hit validation took 0.279 s; Online Mode 0.556 s; Itemless 0.100 s. The Online/Itemless interval is 0.656 s.
- Removed startup timing wrappers from runtime log forwarding and post-exit reporting: the misleading `Startup: -u 9.991 s` measured the running game/log pipe, not initialization.
- First-frame latency from launch remains unknown: CSV time starts at its first frame and the text log has no per-line timestamps. No test suite or build run during review.

### My startup log review, 2026-11-04

- I tested the Linux USA build; my log confirms the new runtime version and a boot-cache hit. Printed preparation stages total 5.250 s; Online Mode plus Itemless account for 0.787 s.
- The largest measured stage is online_config at 2.637 s. Added opt-in substage timings for fresh server settings and nickname checks, preserving request order and current server rules.
- Log ends during Vulkan capability discovery; first-frame/runtime success is not established by this excerpt. No network requests, test suite or build performed in this follow-up.

### Post-pull runtime build, 2026-11-04

- I prepared another Linux runtime build from 9c52580 plus local edits for my manual testing, reusing the existing GPU library.
- Output: `out/sacredfire`, version `v0.7.5-48-g9c52580-dirty`.
- Log and previous binary/version: `out/cpu-build-163.laAs9N/`.
- Compiler/linker completed successfully; GPU SHA-256 remained unchanged and dynamic dependency inspection found no missing libraries.
- New native runtime changes are included. Upstream GPU/launcher C++ changes are not included in the reused GPU library or existing launcher binaries. No test suite or game session was started.

### Upstream integration, 2026-11-04

- Fetched origin/main and fast-forwarded main from 2890858 to 9c52580, including five upstream commits. Reapplied local changes with no Git conflicts.
- Preserved local edits in stash and out/before-main-update-20261104-080619; verified the pre-existing dirty FSR submodule status/diff remained unchanged.
- Retained upstream Itemless caching and Steam Fossilize disablement. Local nested-archive optimizations now run only on cache rebuilds.
- Extended the Itemless stamp to distinguish canonical game roots and resolved mod archive identities; avoided repeated canonicalization in the cache-hit path.
- Direct comparison: upstream hits 7.13–7.47 ms, integrated hits 6.66–7.23 ms. Integrated builds 0.323/0.427 s versus upstream 5.163/52.390 s (large storage outlier disclosed). All eight generated overlays matched.
- Corrected a local import-generator indentation error during source review; modified Python files parse, shell syntax and diff whitespace checks pass. Test suites and builds were not run.
- Existing prebuilt binaries remain from before this pull. New upstream native runtime/GPU changes require their corresponding builds before binary-level validation.
- Detailed conditions and evidence are in STARTUP_PERFORMANCE.md.

### Startup performance follow-up, 2026-11-04

- I observed the long interval between Time Trial and Itemless and investigated the full startup path.
- Traced both launchers and measured preparation against local USA assets, writing only isolated workspace outputs.
- Removed nested PAK temporary copies; reused Itemless lookup sets and replaced repeated IGZ sorting with ordered insertion.
- Itemless: 3.825 s initial, 0.833–0.881 s across three final profiled runs. Online: 1.457 s initial, 1.001–1.097 s final. Generated asset comparisons matched (8 Itemless and 27 Online assets).
- Added optional BB_STARTUP_TIMING diagnostics to both launchers and corrected import-name generation to use the selected boot cache's link.json.
- Added regression fixtures without running the test suite. No additional build or GPU rebuild performed.
- See [startup performance](STARTUP_PERFORMANCE.md) for the plan, measurement conditions, stage map, and outstanding native/runtime validation.

### 2026-11-04

- Confirmed the workspace was on main.
- Read issue 163 and traced Windows/Linux boot preparation.
- Identified boot consumers in patches, widescreen, and online modes; recorded existing OS locking patterns and the native Windows test gap.
- Wrote the design, implementation plan, acceptance criteria, and this tracker.
- At the documentation-only stage, no runtime code was changed and no builds or game tests were run.

Subsequent implementation on the same date:

- Added per-region fingerprint caching, input/output digests, staging, and retention of two fingerprints per region under a session lock.
- Defined analysis.json as an inventory snapshot at boot generation time; live archive damage checks still apply on hits according to region policy.
- Added --boot-dir for consumers writing dynamic output while preserving legacy standalone input paths. got_import requires an explicit selection when a cache exists to avoid silently reading stale legacy output.
- Found that Linux restart closes descriptors starting at 3 in src/probe.c. The Python supervisor retains the lock and uses exit code 75 so restart does not conflict with its own session lock.
- Moved run.sh log opening after lock acquisition and allocated a dynamic descriptor to avoid overwriting the inherited lock descriptor.
- Python/shell syntax checks were run on an early revision before the restriction on tests/builds. Those results do not validate the current revision. No behavior tests or builds were run.
- Added supervisor termination forwarding to the child process group so Stop from the launcher/Steam does not merely terminate the supervisor and leave the runtime running.
- Used an absolute shell/script path for Linux relaunch after changing the working directory.
- Reused Windows extended-length path handling. The cache creates no symlinks; game source symlinks/junctions are resolved, while links at cache directories are rejected.
- Canonicalized the output directory for lock identity too: out aliases through symlinks/junctions must share a lock, including custom CLI output directories.
- Wrote generated-fixture tests for the actual preparation/linking pipeline and process/filesystem tests without running them.
- Added native Windows/Ubuntu CI configuration without pushing changes or dispatching CI.
- Confirmed through source inspection that Windows packaging copies scripts/*.py and AppImage packaging copies the scripts directory. Packages have not been built or launched.
- Documented performance limits: 1 MiB streaming verification and no boot rebuild/write on hits according to the code path; hashing costs and speedup remain unmeasured.
- I keep the design, plan, and progress documents in English. No tests or builds were run for this documentation change.

Authorized runtime-only build:

- Compiled the Linux runtime using the compiler/linker flags from build.sh, without invoking its GPU configuration/build steps or test section.
- Output: `out/sacredfire`; version: `v0.7.5-39-g2890858-dirty`.
- Build log: `out/cpu-build-163.rKUSVK/build.log`. The previous runtime and VERSION were preserved in the same directory as `sacredfire.previous` and `VERSION.previous`.
- GPU library: `out/gpu/libbbgpu.so`. Its before/after SHA-256 check passed; it was not rebuilt or replaced. Dynamic dependency inspection found the expected GPU/SDL3/lzma libraries with no missing-library entries.
- Confirmed the existing game executable at `/home/n0bu/Games/CTR/CUSA13795-1.21/eboot.bin` and existing data/save directory at `/home/n0bu/.local/share/sacredfire` without modifying them.
- Added a prebuilt-mode version lookup beside BB_PROBE so the checkout's out/VERSION is available when no packaged root VERSION exists.
- At this initial build stage, I had not launched the game or executed test suites. Subsequent manual launch results are recorded above.

From the repository root, I launch the existing game/data with the rebuilt runtime and existing GPU library:

```bash
BB_PREBUILT=1 \
BB_PROBE="$PWD/out/sacredfire" \
BB_GAME_DIR="$HOME/Games/CTR/CUSA13795-1.21" \
BB_DATA_DIR="$HOME/.local/share/sacredfire" \
bash run.sh
```

The first launch is expected to generate the new verified boot cache. A subsequent launch with unchanged inputs should log `Boot cache: hit`. I subsequently observed cache hits in my Linux USA launch logs; automated miss/update/recovery coverage remains pending.

## Deferred validation

- Source-only Python suite: 445 tests, 371 passed and 74 skipped. Local native loader fixtures have nine baseline failures, reproduced with the pre-change runtime. Python compilation, shell syntax, XML parsing and whitespace checks passed.
- Boot artifact equivalence and invalidation fixtures passed on Linux. Itemless/Online asset comparisons matched the measured baselines (see STARTUP_PERFORMANCE.md).
- Native Windows behavior, extended-length/UNC paths, locked files, and permissions have not been runtime-verified.
- Linux lock contention, process death, supervisor restart/termination and path aliases passed fixture tests. Read-only AppImage installations and native Windows behavior still need validation; saved logging was exercised during my manual checkout launches.
- Disk exhaustion, power loss, separate volumes, and inputs changed externally during use need additional evidence.
- Preparation timings are recorded; exact first-frame latency, complete I/O counts, peak memory and cache-size benchmarks remain pending.
- This work is being submitted as a draft PR against main. It has not been merged or released.

## Next steps

I completed the Linux automated checks and manual USA/EU launches. Next, verify the Windows CI job, native Windows gameplay, package behavior and the existing native loader fixture failures. GPU and package builds remain deferred. Do not mark Windows/Linux or USA/EU support as verified until the evidence matrix contains actual results.
