# Linux: from your `.pkg` files to a game folder

The launcher needs the game as **folders of decrypted files** (see *Which game dumps work* in the
README). If what you have are the two PS4 packages, `tools/install_from_pkg.sh` does the steps
that otherwise are done by hand:

1. checks both `.pkg` files (whole file, PS4 header, same game and edition),
2. extracts the base game and the 1.21 update with a PKG extractor,
3. merges the update over the base game with `tools/merge_update.py` (hard links: no extra disk space),
4. checks the result with `scripts/game_check.py`, the same check the launcher makes.

You need your own copy of the game: the **base game** `.pkg` and the **1.21 update** `.pkg`, both of
the same edition (USA `CUSA13795`, or EU `CUSA14876`, which is less tested). Nothing is downloaded
except, if you ask for it, the extractor.

```bash
tools/install_from_pkg.sh                                   # looks for the two .pkg files by itself
tools/install_from_pkg.sh --base BASE.pkg --update UPDATE.pkg
tools/install_from_pkg.sh --out /games/ctr                  # default: ~/Games/CTR-NitroFueled
```

It leaves, in the output folder:

```
CUSA13795            the base game, extracted
CUSA13795-patch      the update, extracted
CUSA13795-1.21       the merged game: choose this folder in the launcher (it should say 1.21)
```

Running it again does nothing if the merged folder passes the check, and it reuses the extracted
folders if only the merge was missing. About 35 GB must be free while it works. Keep the output
outside the repository checkout: game files must never be committed.

## The extractor

The script does not bundle a PKG extractor. It uses, in this order: `--extractor PATH`, the
`PKG_EXTRACTOR` environment variable, `pkg_extractor.AppImage` in the current folder, in the output
folder or next to the script, or `pkg_extractor` in `PATH`. It is called as
`EXTRACTOR PKG OUTDIR` and has to create `OUTDIR/CUSA13795` for a base game and
`OUTDIR/CUSA13795-patch` for an update.

`--download-extractor` fetches the AppImage of the `PKG_EXTRACTOR_1_0` release of
[shadPS4Plus](https://github.com/AzaharPlus/shadPS4Plus/releases/tag/PKG_EXTRACTOR_1_0) into the
output folder and prints its SHA-256. It is a third-party program with its own license: if you
prefer, download it yourself and use `--extractor`.

## Problems

| Message or symptom | Cause and fix |
|---|---|
| `… is only N MB` | An incomplete download, or one part of a split archive (`.part1.rar`…). Use the whole `.pkg`. |
| `editions cannot be mixed` | The base game and the update are from different editions (USA `CUSA13795` / EU `CUSA14876`). |
| `swap --base and --update` | The file given as base is the update, or the other way round. The base game is the bigger one. |
| The launcher says 1.00 | The update was not merged: run the script again, or `tools/merge_update.py` by hand. |
| `Some archives/*.pak files have their file-name table filled with zeros` | A bad extraction (see `scripts/game_check.py`). Extract again, with another extractor if it repeats. |
| The AppImage does nothing when opened | It needs FUSE 2: `sudo apt install libfuse2t64` (older distributions: `libfuse2`), then `chmod +x file.AppImage`. |
| Copying the packages to a USB drive fails | Use `cp -r` or `rsync -rh`, not `mv` or `cp -a` (the file system does not keep Linux permissions), and quote paths with spaces. |

Never write an AppImage to a drive with a disk-image writer application: it overwrites the whole
drive, partition table included. An AppImage is run, not burned.

Loading is much faster from an internal drive than from a USB drive. The first races compile
shaders for your GPU and are slower.
