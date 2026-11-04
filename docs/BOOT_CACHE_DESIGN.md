# USA and EU boot cache design

Status: draft implementation available; Linux runtime-only build completed using the existing GPU library. I manually verified Linux USA cache-hit launches and played a race with EU on Linux; Linux cache contracts pass; native Windows validation remains pending. Date: 2026-11-04.

[Issue 163](https://github.com/Ryu7w7/OCTR-NitroFueled-PC/issues/163) reports boot files being rewritten on every launch. The change aims to reuse valid preparation results, refresh them automatically when inputs change, and maintain consistent Windows and Linux behavior.

Related documents: [implementation plan](BOOT_CACHE_PLAN.md) and [progress and validation evidence](BOOT_CACHE_PROGRESS.md).

## Behavior observed during the audit

Audit baseline: `main` at commit `2890858bc583b2126d9889889f039dec10d56244`.

| Component | Finding |
| --- | --- |
| [run.py](../run.py), [run.sh](../run.sh) | Run prepare, link_libc, and link_modules on every launch |
| [prepare.py](../scripts/prepare.py) | Writes boot.bin, eboot.elf, entry.bin, profile.txt, and analysis.json; validates the game and inventories assets |
| [link_libc.py](../scripts/link_libc.py) | Writes boot-libc.bin and libc-link.json |
| [link_modules.py](../scripts/link_modules.py) | Reads boot.bin and game modules; writes boot-linked.bin and link.json |
| [patches.py](../scripts/patches.py) | Reads boot.bin and eboot.elf to select and apply patches |
| [aspect_ratio.py](../scripts/aspect_ratio.py), [widescreen.py](../scripts/widescreen.py) | Read eboot.elf for compatibility checks |
| Online mode paths in run.py/run.sh | Use eboot.elf |
| [isa_rewrite.py](../scripts/isa_rewrite.py) | Already checks the image digest for table reuse; generator changes also need invalidation |
| [mod_session.py](../scripts/mod_session.py) | Provides an OS locking pattern using msvcrt on Windows and flock on Linux |
| [release.yml](../.github/workflows/release.yml) | Python tests run on Ubuntu; a Windows build does not replace native Windows cache behavior tests |

These findings establish the repeated-write path, not its performance cost. Further inspection identified got_import.py as a diagnostic consumer and found that Linux restart in src/probe.c closes descriptors starting at 3. The draft changes below address these findings.

## Behavioral contract

- USA CUSA13795 and EU CUSA14876 have separate cache identities. Executable variants within a region are distinguished by input contents.
- A valid cache skips the preparation parser/linker and does not rewrite boot artifacts.
- Caching does not expand game version support. Existing region, version, and patch validation remains applicable. The EU support marked experimental at the audit baseline remains experimental.
- Relevant input, generator, validation policy, or format changes invalidate previous results.
- Missing or corrupt output triggers rebuilding. A failed rebuild stops launch without falling back to incompatible boot files.
- The cache mechanism does not modify saves or original game files.
- Consumers within a launch must not mix artifacts from different generations or regions.

## Cache identity and validation

The shared implementation is in [boot_cache.py](../scripts/boot_cache.py), with no new dependency. The cache lives in the writable data directory, outside the read-only AppImage installation. Its layout is `out/boot-cache/<title-id>/<fingerprint>/`. The fingerprint also identifies the generation; holding a lock throughout the session removes the need for a separate generation pointer.

The fingerprint uses deterministic serialization of content hashes for eboot.bin, param.sfo, every linked PRX, module order, cache schema version, generator code and dependencies, and options affecting generation or validation. Results generated with `BB_SKIP_GAME_CHECK` must not bypass checks when validation is enabled again.

Content hashes are authoritative, rather than timestamps, sizes, or launcher version alone. Temporary mod overlay directory names do not affect identity when executable and module contents are identical. Any platform or interpreter dependency found to affect output must also be included.

The current generator list is boot_cache.py, prepare.py, game_check.py, link_libc.py, link_modules.py, and mod_components.py for path handling. Linking retains its explicitly little-endian binary format. Cross-platform equivalence still requires testing. The ISA table has a separate stamp covering its generator, prepare.py, iced-x86 version, and table digest.

The manifest records input identity, schema version, required artifacts, output sizes, and digests. Content verification detects corruption that preserves file size. Benchmarks must include the cost of reading inputs and outputs; a speedup is not assumed.

The manifest must not select paths outside the cache directory. Caching does not replace game compatibility validation. Changes to validation policy must invalidate previous validation results.

## Publication and consumer consistency

Build all output in staging on the same filesystem. Verify output and recheck input identity before publication. If inputs change during preparation, discard that result rather than publishing mixed versions.

Publish the complete staging directory, including its manifest, with one rename to the fingerprint directory. Remove a corrupt entry only after its replacement has been verified, under the session lock that excludes active readers. If the process dies between removing the corrupt entry and renaming its replacement, the next launch sees a cache miss and rebuilds. No consumer references staging or falls back to the corrupt entry. Several individual file renames do not constitute an atomic transaction.

Each launch retains its selected generation path. Consumers that previously used one `out` directory for both boot inputs and dynamic outputs receive a separate boot input directory. The runtime, patches, widescreen, and online modes must all receive the same boot path. Do not copy the boot artifacts back into shared `out/` on every launch.

ISA rewrite is a derived artifact with its own boot and generator identity. Dynamic outputs such as online configuration, DLC, patches, and overlays continue to update through their existing paths.

`analysis.json` remains a diagnostic snapshot taken when the boot is built. Its `resource_inventory_scope` field identifies the inventory as a snapshot rather than the current mod/asset state. Asset-only changes do not force relinking; archive damage checks still run on cache hits according to game_check policy. Logs identify the selected cache directory. got_import.py accepts --boot-dir; tools accepting explicit ELF paths use eboot.elf from that directory.

## Locking and failure handling

The OS lock resides at `out/launch.lock` and is acquired before dynamic preparation and before the mod lock. Acquisition is nonblocking: a second launch using the same data receives an error rather than waiting indefinitely. run.py holds the lock through the runtime and restart loop; the cache helper can reenter it within that process. The lock file is not deleted, because replacing its inode could split lock ownership.

Linux uses a Python supervisor that passes a verified lock descriptor to run.sh. The runtime exits with code 75 to request a restart; the supervisor then launches the shell again under the same lock. This avoids the runtime exec path that closes descriptors. The supervisor forwards termination to the child process group and retains the lock while handling shutdown. Signal and restart behavior still requires native testing.

Handle disk exhaustion, denied access, missing inputs, terminated processes, and files held by Windows antivirus. Failed publication must not leave a partial cache accepted as valid. A successful rename alone does not establish power-loss durability; recovery must verify contents again.

The cache creates no symlinks or hard links and does not request administrator privileges. Game source paths may use Linux symlinks or Windows junctions; identity depends on contents. Cache and lock operations reuse the existing Windows extended-length path helper, including UNC paths. Symlinks/junctions at cache directories are rejected so cleanup cannot target another location. Python path handling alone does not establish runtime support for long paths, Unicode, or network filesystems; these remain part of the native test matrix.

PermissionError while reading the cache remains an access error, not a reason to overwrite it. Failed staging or retention cleanup is reported and deferred. An unwritable data folder still requires corrected permissions or location; the cache does not change ACLs, invoke sudo, or silently relocate data.

## Updates and cache lifetime

| Change | Required behavior |
| --- | --- |
| USA or EU game update | Rebuild from new inputs without overwriting the other region |
| Generator or compatibility policy change | Invalidate affected results even if launcher version is unchanged |
| Launcher appearance change only | Do not force a boot rebuild |
| Patch, mod, setting, or DLC change | Refresh affected results; rebuild boot only if its dependencies changed |
| Missing or corrupt manifest/output | Rebuild automatically and log the reason |
| Legacy installation without a manifest | Generate a verified cache; do not assume old files are valid |

Retention keeps the current generation and one other build per region, after successful publication under the session lock. Other regions are untouched. Deletion considers only valid fingerprint names with matching manifest identities; unknown directories and links are skipped. Staging left by an interrupted build is cleaned on the next cache miss. Legacy out files, source game files, and saves are not migrated or deleted automatically.

## Performance and verification cost

A cache hit skips the three preparation/linking stages, their large image allocations, the diagnostic asset inventory, and boot rewrites. Hashing streams in 1 MiB blocks rather than loading all artifacts into RAM. Cache hits still read executable/module inputs and output artifacts to verify integrity. Initial launcher game validation remains; this change does not eliminate all startup parsing.

Measure additional hashing on a miss and read costs on HDDs/SSDs. No percentage improvement or speedup is claimed before benchmarking. Retention limits the number of builds, not their byte size; actual disk usage depends on game images. Gameplay performance and FPS changes are outside this patch's scope.

## Scope limits

Do not change the boot format or linking algorithms in this fix. Do not remove link_libc merely because the main runtime uses boot-linked.bin; that requires a separate compatibility audit. Do not replace existing shader caching or introduce a repository-wide cache framework.

The change is ready to merge only after meeting the [acceptance criteria](BOOT_CACHE_PLAN.md). Native testing on both operating systems and real-game testing for both regions remain necessary; fixtures cannot establish all gameplay behavior.
