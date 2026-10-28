# Corrected widescreen layouts (21:9 and 32:9)

Settings -> Display -> **Aspect ratio** offers 16:9 (the default), 21:9 and 32:9, and then the
screen size: 3440x1440 or 2560x1080 for 21:9; 5120x1440, 3840x1080, 2560x720 or 7680x2160 for
32:9. Both launchers write `widescreen_layout` and `widescreen_output` to bbport.ini. The layouts
and their data are by tcosi (scripts/widescreen_data, nitro_format.py, nitro_components.py,
archive_reader.py); scripts/widescreen.py applies them at each launch, from run.py and run.sh.

A layout corrects what the in-game ultrawide (the monitor's size, `ultrawide=1`) leaves at 16:9:
menu backgrounds, masks and clipping, text and HUD positions, Cup Selection, Hints, Progression
and the boost gauge. Choosing one turns that ultrawide off, since the layout sets the size itself.

## What a launch with a layout does

- **Assets**: 224 base changes in 13 archives per asset set (the two 21:9 sizes have separate sets),
  plus three USA notification entries. Startup text and the USA Wumpa backgrounds are corrected
  within existing entries.
  They are rebuilt from the player's own archives (byte deltas, and copy deltas for the resized Cup
  Selection assets), each checked against its original and result SHA-256, and stored as archive
  patches in `out/widescreen/` (about 9 MB), made once per set. The game reads them through
  `BB_ARCHIVE_PATCHES` (src/runtime_file.c); the archives are never written. The online playlist's
  patch of update2.pak is kept inside the layout's patch of that archive.
- **Executable**: the variant for the internal resolution (render_res 480 to 2160), 16 USA writes
  including racer-name projection, or the existing 15 EU writes,
  located by signature, written to `out/widescreen/patches/Widescreen.xml` and read with the
  bundled patches. scripts/patches.py reports patches that write the same bytes.
- **HUD**: two vertex shaders (0xd4aaf527 and 0x08948e32) get their output x scaled (16:9 over
  the screen's aspect) when their Vulkan module is made, `BB_HUD_SCALE`
  (gpu/.../renderer_vulkan/bb_hud_scale.h). The shader cache keeps the shaders as they are, so
  16:9 and every layout share it. The transform is tcosi's `position_scale`; the C++ one gives
  the same bytes for all 24 variants (checked against the data's SHA-256).
- **Render settings**: scripts/patches.py and the render path read `out/widescreen/bbport.ini`,
  the player's settings with the ones the layouts were made for: render_res 1080 plus
  `layout_internal` (the variant's height), no FSR or DLSS (`BB_TEMPORAL=off`), ultrawide off.
  The player's bbport.ini, saves and shader cache are the usual ones.
- **Scene size**: the variant's lines set the display buffers (for example 5160x2160), but the
  game sizes its 3D targets as 1920x1080 times the render-scale getter (0x13e2eb0 in the 6.72+
  build), which returns 1.0 on the base PS4 path. At internal 1080 the scene is therefore
  1920x1080 stretched to the layout's width. For any other internal resolution,
  `render_plan` takes the Pro path with that height's scale (2.0 at 2160, so a 3840x2160
  scene), removes the Pro output doubling so the layout's size stays as it is, and at 2160
  enlarges the GPU heap as at 16:9 ([13-resolution.md](13-resolution.md)). The scale is the
  same on both axes, so the 16:9 scene is stretched to 21:9 or 32:9 at every internal resolution.

## The European edition

CUSA14876 1.21 gets the same layouts through GetLostIn's port (their commit 403445c,
GPL-2.0), in `scripts/widescreen_data/eu`:

- `ultrawide_eu.json`: the executable runs the same code with two patch sites 16 bytes
  earlier and three constants 64 bytes earlier; `port_meta` moves each line, re-aims its
  RIP-relative and branch operands and gives it the EU bytes as its signature. All 18 variants
  apply their 15 writes to the EU 1.21 executable.
- `assets.json`: the 224 asset changes of each set ported per file to the EU archives (their UI
  files are the USA ones with other strings), checked by SHA-256 like the USA ones. Their port
  shipped the resized Cup Selection file whole (3 entries); here it is a copy delta from the
  player's EU original (`payloads/`), so no game asset is distributed. All 672 read back with
  their expected SHA-256.

## When the game starts at 16:9 instead

The launch says why (`Widescreen: ...`) and goes on at 16:9 when:

- the game's title or update is not USA or EU 1.21;
- a line of the layout's executable patches does not find its one place in the executable
  (`out/eboot.elf`, searched by its signature as `scripts/patches.py` places it);
- an asset is not the expected one (a mod changed it), or the data does not validate.

Online mode does not change the selected layout. The online playlist archive patch is
composed with the layout patches, so both remain active.

The executable is not held to one checksum (PR #46, tcosi): a v0.6 player whose 1.21 build
differed from the recorded one fell back to 16:9. Any build qualifies whose code the patches
fit, so the 5.05 and 6.72+ firmware builds both do. The check is all or nothing for a reason:
`patches.py` skips a patch whose line it cannot place, and the menus and HUD made for the layout
would then show over a 16:9 game. Checked against real dumps: the USA and EU 1.21 executables
fit every line, while the base game's executable under a 1.21 `param.sfo` does not (one signature
matches twice) and keeps 16:9.

## Startup and online interface corrections

Startup/autosave and copyright text keep their proportions by applying the selected
layout's horizontal factor only to their named text placeables in `ui/legal.igz`.

On USA 1.21, the online interface corrections also:

- align connection/lobby notification text with the existing icon and banner, including
  padding after the icon;
- extend only the native blue sprites in Wumpa Challenges
  (`Octane_GrandPrix_Persistent.igz`) and the post-standings Wumpa Coins Report
  (`Octane_AAR_GrandPrixReport.igz`), using explicit bounds and left overscan to hide
  the angled edge. Asset references, menu controls and animations stay unchanged;
- keep racer names above their racers. The CPU camera already projects into the wide
  viewport; the marker hook expands horizontal distance from that viewport's center by
  the reciprocal HUD factor before the centered HUD transform contracts it.

The coins report is a separate layout from `Octane_AAR_WumpaOnly.igz` and the standings.
Neither those standings nor the end-race menu receive additional background changes.
No external background dependencies are added. Archive cache format 13 rebuilds older
cached corrections. The USA path prepares 227 entries and 16 executable writes.

Both 21:9 and 32:9 were user-confirmed working with online enabled on the official
v0.5.4 Windows engine and USA 1.21. Startup text, notifications, racer names and Wumpa
backgrounds were reviewed during that work. This does not certify every output size,
transition, engine build or edition. The online interface asset/projection additions
are USA-only; EU retains its existing layout and executable corrections, with startup
text correction but without a new EU online visual certification.

## Checks

`python -m unittest discover -s tests -p test_widescreen.py`: variant selection for every size
and internal resolution, copy deltas, the HUD list, and archive patches stacked on another patch
(on a synthetic archive). With the game, all 672 rebuilt assets of the three sets read back
through the patches with their expected SHA-256. The USA online corrections were also
checked against actual source assets: named fields only, native bindings/fixups/object
graphs unchanged, and generated archive entries read back as expected. Every USA marker
variant is checked against the executable signatures; the original 12 were also emulated
over 336 viewport/position cases, including preservation of the relocated 21:9 constant
pool. Native Windows frontend compilation passed during the v0.5.4 test build. A full engine rebuild and Linux
live rendering were not performed for these interface corrections.
