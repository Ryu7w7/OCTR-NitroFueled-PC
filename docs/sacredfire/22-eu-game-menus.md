# EU 1.21 game menus

Scope: the game menus and gameplay patches, alongside the private Speed Mode changes in
[21-eu-speed-mode.md](21-eu-speed-mode.md). The experimental native Vulkan renderer is excluded.

## Upstream changes and failure

The Graphics page/restart changes (9f5e080, 7ab5ca3), Skip Intro (b2d72fc), and Quit Game
(2a0ee52, 9c59e40) were checked against CUSA13795 and CUSA14876 update 1.21.
Both launch paths gated the Graphics archive to USA. The archive builder also required USA's
Mexican Spanish/Portuguese tables, which EU does not ship. Enabling the gate alone cannot work:
the EU executable moves several menu functions and the default string argument.

## Changes

- run.sh and run.py prepare the Graphics archive for both supported 1.21 editions.
- Build the text tables actually present: USA en/fr/mx/pt; EU en/fr/es/de/it/ar. Arabic uses
  English labels for the added entries. The menu graph, script and English table remain required.
- Build Quit Game only when its complete graph/script and matching language set are available.
  Its map string remains separate from Online, preserving the fix in 9c59e40.
- Advance the overlay cache version to 19 so previous artifacts are regenerated.
- Validate the region query, all hook/helper entry points and default-string reference before
  installing any Graphics hooks. EU shifts Open2, LanguageItemEvent, LoadMap2, Release, Intern
  and Unref by -0x10; StringPinDefault moves from 0x1df8d10 to 0x1df8cd0. Hide, Show,
  PinGetter, StringPin and the data globals retain their USA addresses.
- The existing page callbacks handle resolution, ultrawide, upscaling/quality, FPS unlock,
  motion blur, FPS limit, saving and restart through these selected hooks.

Skip Intro already matches movie filenames independently of region. Its on/off behavior and
Graphics archive readiness/real-update3 precedence now have runtime regression coverage.
The existing 60 FPS, no motion blur, Pro 1080p and unlock patches already provide EU entries;
compiling them against both local executables produced 1, 1, 1 and 24 writes respectively.
The signed resolution (1620p/2160p), ultrawide (3440x1440), and golden-theme patches also
resolve against both executables (1, 2, 2 and 1 writes); their EU relocation needs no change.

## Validation and limits

`BB_TEST_GAME=/path/to/CUSA14876-1.21 python3 -m unittest discover -s tests -p test_graphics_tab.py -v`
also accepts CUSA13795. It builds the overlay from locally owned files, checks tab visibility,
labels, separate Quit/Online references, and validates hook installation against the executable.
Synthetic tests cover both layouts, bad helpers/default references, disabled settings and short
images. No game assets or generated overlays are checked in.

These checks do not substitute for navigating the page, applying/restarting settings, selecting
Quit Game, or running a private multiplayer race in a live EU session. Windows execution and
those interactions still need manual verification. A local Linux AppImage can use the rebuilt
Graphics hook in its executable (exported to the existing GPU library through ELF symbol
interposition); a normal complete build compiles this source directly into the GPU library.
