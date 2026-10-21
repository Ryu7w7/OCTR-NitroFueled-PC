# Contributing to SacredFire

## Game files

SacredFire contains no game data and accepts none. Do not attach, paste or link eboot.bin,
sce_module files, saves, dumps, decryption material or anything else from the game folder in
issues, pull requests or discussions; such content is removed and the issue closed. The project
does not help with obtaining or dumping the game. You need your own copy of
*Crash Team Racing: Nitro-Fueled* (CUSA13795) and its 1.21 update.

Generated files derived from the game (`out/`, `user/`, logs of the game's own output) stay out
of the repository too; `.gitignore` covers the usual places.

## Bug reports

Use the bug report template. The log is what matters: the launcher's *Log* tab or the file from
*Save the log to a file*, or `run.sh`'s output. `BB_FRAME_STATS=1` adds the `Frame stats:` lines
needed for performance and rendering issues; for a crash, everything from `STOP` or `Guest fault`
to the end. Name your GPU and driver.

## Changes

- Build with `bash build.sh`; run `bash build.sh --test` and `python3 -m unittest discover -s tests`
  before a pull request. The GPU library takes about 40 CPU-minutes to build; the Python tests
  run in seconds and also run in CI.
- Changes inside `gpu/shadps4/` (the vendored shadPS4 video core) are marked with a `bbport:` or
  `sacredfire:` comment, as the existing ones are, so the vendored code can be compared with
  upstream later.
- The runtime (`src/runtime_*.c`) implements what the game calls. A new import gets a real
  implementation or an honest, logged stub that fails visibly; no blanket "return 0".
- Keep a note in `docs/sacredfire/` for anything that took investigation (what was observed,
  what was tried, what fixed it). `STATUS.md` there is the summary of where things stand.
- Commit messages say what changed and why. No generated trailers.

## Releases

The AppImage is a GPL binary, so each published one has to match a published commit:

1. Commit everything and tag the commit: `git tag vX.Y && git push origin vX.Y`.
2. Build from that checkout (`bash build.sh` inside `nix-shell shell.nix`, as the header of
   `packaging/appimage.sh` describes); `out/VERSION` then reads `vX.Y`.
3. `bash packaging/appimage.sh` refuses an untagged, unbuilt or uncommitted tree and writes
   `dist/SacredFire-x86_64.AppImage` with its `.sha256`. The launcher's title bar, `run.sh`'s
   first lines and `VERSION` inside the package show the tag.
4. Create the GitHub release for the tag `vX.Y` (not for a branch), attach the AppImage and its
   checksum, and name the tag in the notes.

## Licenses

SacredFire is GPL-2.0-or-later (see `LICENSE`), as are bbport and shadPS4 it is built on.
Contributions are accepted under the same license. Third-party components keep theirs
(README, *Credits and licenses*). The game patches in `patches/CrashTeamRacing.xml` are community
work by illusion and TroyWarez from [illusionyy/PS-Game-Patch](https://github.com/illusionyy/PS-Game-Patch);
keep the attribution when touching them.
