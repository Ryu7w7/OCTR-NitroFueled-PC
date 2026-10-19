# TODO

Open work for this fork (EU edition and ultrawide). Written so a person or an AI assistant can
pick an item up without the history; addresses are image offsets (no 0x400000 base) unless noted.

## Ultrawide: keep videos, cutscenes and the title screen at 16:9

**State:** ultrawide works in gameplay (scripts/patches.py `OUTPUT_SIZE_SIGNATURE`; the game draws
at the monitor's size and the camera follows its aspect). Three things look wrong at 21:9:

1. **Bink videos** (Activision/Beenox logos, `*.bk2`): stretched sideways to fill the width.
2. **Title screen** (the 3D "Press X to begin" set): built for 16:9, so the sides show the void and
   sky beyond the set.
3. **In-engine cutscenes** (adventure mode, `h100_hub_*_intro.pak` and friends): staged for 16:9,
   expected to show off-stage areas like the title screen. Not yet seen on screen.

**Wanted:** these at 16:9 with black bars (pillarbox), gameplay at 21:9. Optional setting.

**Plan:** the game keeps rendering at the ultrawide size; the presenter
(gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp) shows the frame differently while a
"16:9 moment" is active:

- video: squeeze the full frame into a centered 16:9 rectangle (the video was stretched to fill);
- title screen / cutscene: crop the centered 16:9 part of the frame (the 3D view is correct, only
  wider than the set).

**Signals found so far (EU 1.00 executable; re-find them for 1.21, the code moves):**

- Videos: the game opens `archives/*.bk2` when one starts (`BB_FILE_TRACE=1` shows
  `octane_intro_logo_atvi.bk2`, `openinglogo.bk2`). Track open handles to `.bk2` files in
  src/runtime_file.c; a video plays while one is open. The Bink audio stream (an s16 audio port
  opened by thread `Bink Snd`) is a second signal.
- Cutscenes: `CCutsceneManager` (Alchemy reflection). Its field-name table is at 0x23eb980
  (39 names), field offsets at 0x1c85510 (u16 each). Relevant fields: `_activeCutsceneData` +0xa0,
  `_activeDesignerCutscene` +0xa8, `_paused` +0x59, `_camera` +0x60. The static `_Instance` is very
  likely the global at 0x26b5c68 (right after the class's metafield storage 0x26b5b30..0x26b5c60;
  read by a cluster of functions around 0x98a000). At the title screen `_Instance` was non-null and
  both active fields were 0. Not yet confirmed during a playing cutscene: start a new adventure on
  an empty save slot (the Oxide intro plays) and watch +0xa0 change.
  How to read guest memory from outside: the game image is mapped at 0x800000000 in the
  `sacredfire` process; /proc/PID/mem is readable by a parent process (ptrace_scope 1).
- Title screen: no signal yet. Ideas: the start screen's camera object, or "no player kart and no
  cutscene" from game state; `Octane_StartScreen.pak` loads at boot and stays loaded, so file
  opens do not mark it.

**Then:** the runtime exposes the state (e.g. an atomic the presenter reads each frame, as
BbOverlay does with its settings); a bbport.ini key `ultrawide_cinematics=pillarbox|full` and a
menu checkbox; tests for the patch side.

## Ultrawide: title screen logo and "Press X to begin"

**State (EU 1.21):** src/runtime_frontend.c gives the title screen and the kart customization
screen the horizontal field of view of 16:9 (title 79.755 -> 63.742 degrees, customization
36.739 -> 27.762 at 3440x1440), so their 16:9 backdrop fills the width. The menus are 2D and keep
their ultrawide layout. On the title screen the 3D logo grows by about 1.34 and its bottom
("Nitro-Fueled") now overlaps the 2D "Press X to begin". Narrowing less does not help: the backdrop
covers only 75.5% of the ultrawide width (16:9 is 74.4%).

**Done (2026-10-17):** the logo sits higher on the title screen in ultrawide, clear of "Press X to
begin": src/runtime_frontend.c turns the store of the camera's position (`vmovups [rbx+0xa0],xmm3` in
the camera update, 0xaf7326 in EU 1.21; patches.py --print-frontend's fourth value) into a jump to a
page next to the image that adds an offset vector first: zero, except on the title screen in
ultrawide, -12 along matrix row 0x90. Rows found by trying each on the title screen: 0x70 forward
(zoom), 0x80 sideways, 0x90 vertical (pointing down on screen). Crash's ears touch the top a little
more. (Wanted was: the logo a little higher on the title screen only.)

**How the field of view works (found 2026-10-16 with a hardware watchpoint on the render camera
and Ghidra):** the main render pass (`CPreRenderUpdatePass`, `_cameraName` "main",
`_renderCamera` +0x168) has a render camera with the horizontal field of view in degrees at +0x3c,
the aspect at +0x40, near/far at +0x64/+0x68 and the position at +0xa0. One function (0xaf7110 in
EU 1.21) writes them each frame: +0x3c = camera setup's radians (+0xd0) * 57.29578 (the
instruction at 0xaf7278, which runtime_frontend.c points at its own factor), +0x40 = window
width/height * `_cameraAspectRatioMultiplier`. Readers (0x355c5c frustum, 0x960a04, 0xb0363b)
take tan(fov/2): the sideways extent is tan, the vertical tan/aspect (so +0x3c is horizontal).
Next: find where the position (+0xa0) or the view direction is written in the same way (a
watchpoint on +0xa0) and lower the camera a little while the title screen shows.

## Ultrawide on USA 1.21 (CUSA13795)

`OUTPUT_SIZE_SIGNATURE` was taken from the EU 1.00 executable. On another build it applies only
where the signature matches once; otherwise patches.py logs "patch site ... not found" and skips
it. Check it on a USA 1.21 dump and on EU 1.21.
