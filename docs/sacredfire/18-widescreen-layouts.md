# Corrected widescreen layouts (Windows, based on v0.5.3)

Settings -> Display lists Resolution first and Aspect ratio directly below it.
16:9 is the factory default. 21:9 and 32:9 select the complete menu/HUD correction
set instead of stacking another projection over the built-in widescreen hooks.
The optional output selector supports 3440x1440 and 2560x1080 for 21:9, and
5120x1440, 3840x1080, 2560x720 and 7680x2160 for 32:9. Internal rendering follows the
selected resolution up to 1440p. Fully exit and launch again after changing rendering.

The layout data contains 224 asset adjustments across 13 archives per asset set,
15 executable writes per rendering variant, and two centered-HUD shader transforms.
It addresses menu backgrounds, masks/clipping, text/HUD positioning, Cup Selection,
Hints, Progression and boost-gauge presentation. Different 21:9 proportions have
separate asset sets. This does not certify every screen, transition or resolution.

## Preparation and rollback

The user's archives are read only. Preparation copies the game into a private
`widescreen/profiles` view, validates original asset checksums, applies the deltas,
and checks every reconstructed result. The game executable remains unmodified;
selected XML writes are compiled and checked for completeness/overlap before play.
Allow approximately one full game copy plus headroom per asset set used.

Original v0.5.3 shaders are generated in a private 16:9 setup if missing. After their
expected checksums match and stabilize, only that setup's owned child game receives
a normal close request. The chosen layout then starts using a private patched
cache. No precompiled shaders or game files are distributed by this change.

The source includes byte deltas and copy-delta recipes. A resized Cup Selection
asset is reconstructed from ranges of the user's original asset and small changed
byte sequences, rather than bundling complete IGZ files. `nitro_components.py`
validates original/result hashes and refuses hardlink writes. The reused `nitro_*`
helpers are internal implementation code; no mod ZIP or separate mod toggle is
needed to use this launcher feature.

Widescreen saves are separate under `widescreen/user`, with backups on clean exits.
The first widescreen session can copy existing saves without modifying them.
Selecting 16:9 returns to the ordinary launch path; private profiles are retained.
The upstream render/frontend/cinematic hooks are suppressed in the widescreen
session's private INI only. The user's base INI remains the source of preferences.

## Supported scope and verification

This contribution targets the native Windows launcher. The Python/Linux launcher
and `run.sh` keep their upstream behavior. The current gate accepts the tested
USA 1.21 executable and official v0.5.3 Windows engine; shader checks remain exact.
Other engine builds and European asset sets require explicit porting/verification.
Online and loose-file mod combinations are intentionally blocked for this initial
integration. 16:9 ordinary launches retain upstream behavior.

Both 21:9 and 32:9 were live-reviewed on official v0.5.3 and closed normally (exit 0).
The user also approved the final Resolution/Aspect ratio control arrangement.
Static checks reconstructed all 672 assets across the three asset sets, verified
all 12 XML variants with 15 complete/non-overlapping writes each, and checked all 24
shader transforms against actual v0.5.3 generated original shaders. This is not a
full visual regression matrix or an online/multiplayer compatibility claim.

Run `python -m unittest discover -s tests -p test_widescreen.py` for the source-only
selection, rendering-conflict, restart and copy-delta regression tests. They do
not require game data. The complete portable packaging recipe includes the layout
data directory. Native frontend compilation was tested with LLVM-MinGW/C++20 and
the pinned ImGui; the official engine/DLLs were used unchanged for live testing.

Author: tcosi. Code and original patch recipes: GPL-2.0-or-later, consistent with
this repository. Read-only archive layout reference: kishimisu/Crash-NST-Modding-Tool
`src/pak`. Original game assets are required locally and are not included.
