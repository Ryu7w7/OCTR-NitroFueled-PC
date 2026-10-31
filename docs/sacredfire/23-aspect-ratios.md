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

Every write must match one executable signature before the files are published.
Unsupported editions, missing or ambiguous signatures, and malformed projection
data fall back to 16:9. The generated patch is `out/aspect_ratio/patches/AspectRatio.xml`;
the temporary settings are `out/aspect_ratio/bbport.ini`. The saved preferences and
source executable stay unchanged.

16:10 keeps the original menu assets, HUD shaders and video code. It clears a prior
`BB_HUD_SCALE` override and keeps existing archive patches such as the online playlist.
FSR/DLSS and raw ultrawide are disabled in the private launch settings. The existing
`layout_internal` scene scaling supports 480p through 2160p; the display size follows
the selected 16:10 output, while the internal targets retain their authored 16:9 shape.

## Verification and limits

Automated checks cover selection and old-setting compatibility, both editions, all
three 16:10 outputs and six internal resolutions, strict patch compilation without
overlaps, signature and projection-data fallback, the shell launch plan, and execution
of the camera-copy instructions to check pixel proportions. C++ checks cover settings
load/save precedence, all four in-game choices, output-size changes and restart detection.

Live menu/HUD, split-screen, subtitles, video proportions and online verification remain
pending; videos may stretch. Existing 21:9/32:9 visual confirmations do not certify 16:10.
