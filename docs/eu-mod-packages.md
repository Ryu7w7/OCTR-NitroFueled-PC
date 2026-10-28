# EU declarative mod packages

This update adds **CUSA14876 / 01.21** support to the existing declarative mod loader.
It does not replace the native launcher executable or bundle any game data.

- Package XML must name exactly one supported CTR edition. At preparation time,
  its title/version must match the actual game's `sce_sys/param.sfo`.
- Selection, generated combined XML, strict executable preflight and cached profile
  identity use that actual target. USA packages remain supported on CUSA13795 01.21.
- The display-only `OCTR-Mod-Library.xml` catalog is region-neutral. The native
  launcher assigns multi-ID XML files their first ID for display and then filters
  by game edition; listing USA first incorrectly hides the package on EU. Catalog
  entries contain no executable writes. Executable package XML remains region-gated.
- Missing SFO, wrong version and cross-edition packages are rejected. Seven-write
  synthetic regressions exercise the real adapter/compiler for both USA and EU.

## Compatible mod

[CNK Drift Gauge + Reserves Meter — EU adaptation](https://github.com/Majed-allababedi/CTR-CNK-Drift-Gauge-EU)

Original mod by **tomtotom**: https://gamebanana.com/mods/729041

EU adaptation and local gameplay confirmation by **Majed-allababedi**.

**Select Speedometer HUD in-game.** Classic/Nitro-Fueled are not the intended HUD
for this combined gauge package. Disable overlapping gauge mods and back up saves.

## Updating a compatible portable installation

These changes are in four Python modules in `scripts/`:

- `mod_format.py`
- `mod_content.py`
- `mod_library.py`
- `mod_packages.py`

They target the mod framework already present at this repository's base commit
`30611310865f0d3e98251d38914a43177df56f47`. Back up those files, then use their versions
from this update in the corresponding compatible installation. Do not overwrite a
newer/differently modified loader blindly. Restart the launcher and press **Reload**.
The native `.exe`, game executable and saves do not need replacing for this update.

## Verification boundary

The four-module source change preserves the tested local EU implementation, with
synthetic tests added for publication. Tests cover native catalog filtering semantics,
exact SFO matching and all seven writes surviving actual adapter preflight. The existing
synthetic package fixture now includes a minimal SFO; its source-preservation assertion
recurses into that new fixture directory rather than removing the check.

The full Python suite on this Windows host has known failures/errors in Linux Vulkan
path handling, Windows symlink privileges and intermittent temporary-directory cleanup;
these were also present in the unmodified base. Do not interpret this note as a passing
full-suite or native rebuild claim. The new regional suite passes all eight tests.
