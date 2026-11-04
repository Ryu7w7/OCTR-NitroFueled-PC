# Boot cache implementation plan

Startup follow-up: [startup performance plan and progress](STARTUP_PERFORMANCE.md)
tracks the full preparation path and measured Online/Itemless optimizations.

Status: draft code and tests written; authorized Linux runtime-only build completed. Linux automated tests have been run; GPU rebuilds and package builds remain deferred. Date: 2026-11-04.

This plan implements the [boot cache design](BOOT_CACHE_DESIGN.md) for issue 163. Record each stage's results in the [progress tracker](BOOT_CACHE_PROGRESS.md). Complete dependencies before marking subsequent stages complete.

Implementation has continued through static inspection, but baseline measurements and validation requirements remain incomplete. Priorities are data safety, process conflicts, permissions, Windows/Linux paths and links, and performance. I use a runtime-only build with the existing GPU library and game/data directories for my manual testing. Linux automated tests have been run; GPU rebuilds and package builds remain deferred.

## Stage 1 Complete the audit and establish a baseline

- Inventory all boot, metadata, and ISA table readers/writers, including diagnostic tools and direct entry points.
- Record generator dependencies, environment options, game validation, and runtime format requirements.
- Audit session and restart lifetimes and shared outputs; define lock acquisition order.
- Define separate boot input and dynamic output directory parameters, analysis.json handling, and retention policy.
- Record the original pipeline's results for available USA/EU executable variants: output digests, timings, bytes read/written, game/port versions, OS, and storage type.

Required result: a complete dependency map, detailed design decisions, and a comparable baseline. Document the technical decision on consumer path separation before integrating it.

## Stage 2 Implement cache contracts and tests

- Create SELF/ELF, PRX, SFO, and data directory fixtures without committing proprietary game assets.
- Add meaningful tests for fingerprints, output validation, publication, invalidation, and recovery.
- Implement the shared cache module with deterministic manifests, staging, and OS locking.
- Verify inputs before and after generation; reject publication when inputs change.
- Log cache hits, misses, rebuilds, and failures concisely, without unnecessary per-byte or per-file logging.

Required result: output matches the original pipeline and failure tests pass. A cache hit neither invokes generators nor modifies boot artifacts.

## Stage 3 Integrate launch paths and consumers

- Connect run.py and run.sh to the same implementation.
- Ensure the runtime, patches, aspect ratio/widescreen, online modes, and ISA rewrite use the session's selected generation.
- Preserve updates to DLC, patches, mods, online configuration, and diagnostic metadata according to their inputs.
- Prevent a second launch sharing data from corrupting dynamic output; test restarts and lock release.
- Preserve affected tool and entry point compatibility with minimal changes and document path changes.
- Test migration from legacy out directories and cleanup that leaves user files untouched.

Required result: USA → EU → USA selects the correct artifacts and reuses valid cached results.

## Stage 4 Validate native platforms and packaging

Add native Windows and Linux cache tests to CI. Retain existing source checks. Windows must exercise filesystem and process operations using native Python, not only bash/MSYS or cross-compilation.

Verify inclusion of new scripts in the portable ZIP and AppImage, writable data directory use, bundled Python, paths containing spaces/Unicode, and game/data directories on separate volumes.

| Area | Required cases | Passing evidence |
| --- | --- | --- |
| Equivalence | Original versus new pipeline, USA and EU | Identical boot artifact digests for identical inputs |
| Reuse | First and subsequent launches | No boot artifact writes or linker calls on a hit |
| Regions | USA → EU → USA; variants within a region | Correct selection and reuse of valid existing results |
| Updates | Eboot, each PRX, SFO, generators/dependencies, schema | Invalidation follows dependencies |
| Validation | Bypass enabled then disabled; policy changes | Cache does not bypass current validation rules |
| Fingerprints | Different contents with unchanged size/mtime | Change is detected |
| Corruption | Malformed manifest, missing/truncated files, same-size corruption | Rebuild or clear error without stale fallback |
| Interruption | Kill during generation/publication; inputs change during generation | Partial generations are never accepted |
| Filesystem | Disk full, access denied, locked files | Safe failure and recovery on a later launch |
| Concurrency | Two processes, restart, death of lock owner | No corruption, deadlock, or abandoned lock |
| Paths | Spaces, Unicode, supported path lengths, junctions/symlinks, separate volumes | Existing application support boundaries are preserved |
| Integration | Patches, widescreen, online, DLC, mods on/off, ISA rewrite | Correct boot region and current inputs |
| Retention | Migration, repeated updates, cleanup during active sessions | User files and active generations remain intact |

Test actual packages with USA/EU on Windows and USA/EU on Linux. Record game builds, launch paths, renderers, and exercised features. Mark unavailable combinations as untested, not passed.

## Stage 5 Benchmark and review

Measure preparation time separately from time until the game appears. Compare the original pipeline, misses, hits, updates, and region switches on the same machine with the same inputs. Record repetitions, median/range, OS cache conditions, cache size, bytes read/written, and input/output hashing cost.

Review the diff and every changed caller. Do not add timestamp shortcuts or weaken integrity verification merely to improve benchmark results. If hashing slows launch on target storage, revise the design based on measurements and repeat affected tests.

## Merge acceptance criteria

- All consumers use the same boot generation; no stale consumer path remains.
- Boot output matches the baseline for identical inputs.
- Hits do not rewrite boot artifacts; relevant updates trigger rebuilding automatically.
- Failure, recovery, and concurrency tests pass natively on Windows and Linux.
- Relevant existing tests show no regressions.
- Real USA/EU package tests on both operating systems have evidence; missing evidence prevents a cross-platform readiness claim.
- Benchmarks demonstrate measurable benefit without unresolved material performance regressions.
- Retention, migration, and support boundaries are documented.

No percentage speedup or zero-bug guarantee is made before testing. Any unmet criterion keeps the change unready for merge. Available draft code is not evidence that tests passed; the draft PR does not establish merge readiness.

## Automated validation and remaining checks

[test_boot_cache.py](../tests/test_boot_cache.py) provides generated ELF/SFO fixtures to compare the original pipeline against caching and exercise invalidation, corruption, recovery, retention, and process locking. [test_boot_cache_paths.py](../tests/test_boot_cache_paths.py) covers paths/links, patch consumers, and ISA table integrity. These fixtures do not replace real-game testing.

The release workflow now defines boot-cache-checks for native Ubuntu/Windows Python and makes it a dependency of package builds. Editing the workflow does not mean CI has run; this work has not pushed changes or dispatched a workflow.

Linux fixture and source-only tests have now passed; see BOOT_CACHE_PROGRESS.md for counts, skips and existing native-loader baseline failures. Native Windows and actual package tests remain pending. Actual antivirus locks, full disks, separate volumes, network filesystems, and forced termination at multiple stages require additional environments and testing beyond the written fixtures.
