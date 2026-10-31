# Aspect ratio selection and experimental 16:10 support

Settings -> Display -> **Aspect ratio** offers 16:9 (default), experimental 16:10,
21:9 and 32:9. The in-game Graphics page offers the same four ratios, applied after
a restart. Both launchers and the game save the same settings:

```ini
aspect_ratio=16x10
aspect_output=1920x1200
```

16:10 offers 1280x800, 1920x1200 (default), and 2560x1600. Changing the ratio resets
an incompatible output size. The existing `widescreen_layout` / `widescreen_output`
preferences remain accepted when the new keys are absent; new keys take precedence
regardless of their order in the file. Saving a 21:9 or 32:9 choice also updates
the older keys for compatibility. A 16:10 choice sets `widescreen_layout=off`.

## Preparation

`scripts/aspect_ratio.py`, called by `run.py` and `run.sh`, selects the display shape.
16:9 uses the original game. 21:9 and 32:9 continue through the existing
[corrected widescreen layouts](18-widescreen-layouts.md), including tcosi's menu
assets and centered HUD. Their asset preparation and patch data are unchanged.

16:10 has its own path and `scripts/aspect_ratio_data/16x10.xml`. It uses the existing
output-size loads and adapts tcosi's projection-copy replacement with factor
`(16/9)/(16/10)`. Its relative loads and final jump stay inside the replacement, so
signature relocation preserves them on supported USA and EU 1.21 builds. The four
projection-row X components change; vertical field of view stays the same.

The second signed write corrects `igGuiPlaceableBolt`'s GUI-to-effect conversion.
Before camera unprojection, it multiplies all three corners' normalized X by 0.9.
This compensates for the camera's 10/9 X factor, keeping 3D HUD effects aligned with
their 2D sprites and text. Both position and width are corrected, so a charging
effect follows the animated fill instead of extending ahead of it. The correction
uses the current canvas and placeable transform, including host scaling and
animations. Its relative loads and jump stay inside the replacement; the following
native matrix-transform call is retained. Readable assembly is included alongside
the XML in `scripts/aspect_ratio_data/16x10-vfx.s`.

Every write must match one executable signature before the files are published.
Unsupported editions, missing or ambiguous signatures, and malformed projection
data fall back to 16:9. The generated patch is `out/aspect_ratio/patches/AspectRatio.xml`;
the temporary settings are `out/aspect_ratio/bbport.ini`. The saved preferences and
source executable stay unchanged.

16:10 keeps the original menu assets, HUD shaders and video code. It clears a prior
`BB_HUD_SCALE` override and keeps existing archive patches such as the online playlist.
Its HUD corrections are prepared separately by `scripts/aspect_hud.py` from the
player's archives, through the same archive overlay mechanism as the existing layouts.
Adventure collectible assets and bobbing-animation keys remain unchanged; their
spacing is handled by the GUI VFX conversion above. The shared boost gauge's master
container gets a 0.9 Y scale,
with its bottom edge preserved; its fill, cursor and effects keep their hierarchy.
The charging effect also uses the existing wide layouts' named-field size correction.
No whole HUD asset is distributed, and every other asset byte and reference stays intact.
Unexpected HUD structures or overlapping asset replacements reject the 16:10 plan
instead of overwriting a mod. Without archives, executable-only callers can still
prepare the display/camera plan.
FSR/DLSS and raw ultrawide are disabled in the private launch settings. The existing
`layout_internal` scene scaling supports 480p through 2160p; the display size follows
the selected 16:10 output, while the internal targets retain their authored 16:9 shape.

## Verification and limits

Automated checks cover selection and old-setting compatibility, both editions, all
three 16:10 outputs and six internal resolutions, strict patch compilation without
overlaps, signature and projection-data fallback, the shell launch plan, and execution
of the camera-copy instructions to check pixel proportions. C++ checks cover settings
load/save precedence, all four in-game choices, output-size changes and restart detection.

HUD checks cover the five applicable EU boost asset copies, bottom-edge and pixel-proportion
preservation, unchanged references, archive-overlay composition/cache behavior, and
unexpected-data rejection. The GUI VFX instruction checks cover three corner points,
animated fill positions, canvas sizes, and preserved Y/depth/W components.
EU 1.21 Adventure was visually checked at 2560x1600 with a 1440p internal scene: all three
collectible/count pairs align, the icons keep their bobbing, and the shared boost frame
has its corrected height. The player also checked the active drift gauge and confirmed
that its charging effect now aligns with the fill.

Race transitions, split-screen, subtitles, video proportions and online HUD verification
remain pending; videos may stretch. Existing 21:9/32:9 visual confirmations do not certify
16:10. The shared boost asset is also used by race HUDs; the live check above is Adventure.
