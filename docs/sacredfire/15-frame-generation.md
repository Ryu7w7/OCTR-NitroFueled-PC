# FSR 3 frame generation in CTR (experimental)

Setting `frame_generation` (menu and launcher: *Frame generation (FSR 3, experimental)*, off by
default; shown with temporal upscaling FSR 3 or DLSS). `BB_FRAMEGEN=1/0` overrides it.

One interpolated frame is shown between two game frames, so the screen gets twice the frames the
game draws. It uses FSR 3.1's frame interpolation from the FSR-Vulkan submodule (its portable C
API, `ffxVkPortableFrameGeneration*`: optical flow plus the game's motion), the same library and
the same inputs as the FSR 3 upscaler ([14-fsr3.md](14-fsr3.md)). Nothing new is linked.

## How it runs

1. `CtrFsr3::Upscale` (FSR 3 or DLSS) publishes what the interpolation needs from the frame it
   just upscaled: the upscaled frame before the HUD (output size), the R32F depth, the pixel
   motion, the jitter, near/far/FOV and the camera axes and position (the inverse of the view
   matrix in the G-buffer constants). `TakeFrameGenInputs` hands them over once.
2. The presenter's frame recording (`PrepareFrame`), after the post-processing pass drew the real
   frame (with the HUD) into the presentation frame, takes those inputs and calls
   `CtrFrameGen::Record`:
   - the HUD-less frame goes through the same post-processing pass into an image of the same
     size and format (per presentation frame). It is a copy of the display buffer taken at the
     display pass's second draw (`CtrFsr3::CaptureHudless`): the first draw is the game's own
     stretch of the upscaled frame, the others are the HUD. A first version used the upscaler's
     output instead, which the game's stretch shader changes: every pixel differed from the real
     frame, FSR 3 took the whole frame as HUD and the "interpolated" frame was the real one (the
     counter doubled, the motion did not get smoother);
   - optical flow and the interpolation's preparation run on the HUD-less frame, depth and
     motion; the dispatch gets the real frame and the HUD-less one, so FSR 3 takes the HUD from
     the real frame instead of moving it with the scene;
   - the interpolated frame (between the previous real frame and this one) is written to an
     RGBA8 storage image of the slot.
3. `Present` shows the interpolated frame at once, waits until half a game frame later (an
   average of the time between game frames, at most 25 ms) and then shows the real frame (see
   Limits for when an interpolated frame is dropped).

The first frame of a sequence only prepares: the interpolation starts again (and that frame shows
alone) after a frame without the upscaler (menus, loading screens, videos), a pause over 100 ms,
an upscaler reset, a resize or a change of render size.

## Limits

- Needs FSR 3 or DLSS as temporal upscaling (the inputs come from there). Not with HDR output or
  the presenter's own FSR pass. Menus without a 3D scene show only real frames.
- Latency: the real frame goes out half a game frame later than without interpolation.
- The FPS limit applies to the game's (real) frames only; frame generation adds one frame per
  real frame on top, whatever the screen's refresh rate. Interpolated frames never hold the game
  back: one is dropped when no swapchain image is free at once (`TryAcquireNextImage`) or when
  the game already waits for a presentation frame, and the half-frame wait before the real frame
  ends as soon as the game needs a frame. A first version that waited for the screen took a
  120 FPS game to about 80 FPS on a 144 Hz screen. With vsync the screen still shows at most
  its refresh rate; Mailbox keeps the newest frame, Immediate shows them all with tearing.
- The interpolation itself costs GPU time (the HUD-less pass, optical flow, interpolation): on a
  GPU already at its limit the game's own rate drops by that much.
- The FPS counter counts the frames shown; with frame generation it reads
  `N FPS (game M, FG: On)`.
- Moving karts: their own motion is not decoded (the velocity target, see 14-fsr3.md), so FSR 3
  blended the frames before and after on them: double images, worst on the turning wheels. The
  upscaler's inputs pass now also writes the velocity target's moving-objects mask (R8, render
  size; this frame's and the previous one alternate), and `ctr_framegen_compose.comp` puts the real
  frame's pixels where either mask is set (dilated by 3 render pixels with a soft edge) into the
  interpolated frame. The karts then move at the game's own rate, without ghosting; the world
  around them is still interpolated.
- Around the karts: the background the camera's motion uncovers or hides next to them between two
  frames got the karts' zero motion from FSR 3's dilation and was smeared (a halo around the kart,
  wider at speed). The compose pass also takes the real frame where a kart lies along a pixel's
  own motion within 1.25 frames of it, so that band follows the speed.
- The velocity target is the motion blur's input: with the "Disable Motion Blur" patch it stays
  empty, the mask with it, and the steps above did nothing (the ghosting stayed). A second mask
  (`ctr_framegen_mask.comp`, after the inputs pass) compares this frame with the previous one
  (render size, before the upscaler, kept in a copy) fetched where this frame's motion says each
  pixel was, best of 3x3 for the jitter: where it does not match (largest channel difference over
  0.12, `BB_FRAMEGEN_ERROR`) the motion does not explain the picture (karts, turning wheels, the
  background next to them, particles) and the pixel joins the mask.
- First in-game views (`BB_FRAMEGEN_MASK=2`) showed the kart covered, but also thin lines on
  high-contrast edges of the road and walls (a pixel or two of motion error) and a band from the
  kart down to the screen's edge (the search along the motion went 1.25 frames each way). Now the
  comparison's tolerance grows with the speed (3x3 to 7x7), the raw result goes through
  `ctr_framegen_mask_clean.comp` (a pixel stays when 35% of those within 2 are set,
  `BB_FRAMEGEN_DENSITY`) and the search along the motion reaches 0.6 frames (the interpolated
  frame is half a frame from each real one).
- `BB_FRAMEGEN_MASK=0` turns both off, `=2` tints the real-frame area magenta;
  `BB_FRAMEGEN_MASK_RADIUS` (0-8, default 3) sets the margin and `BB_FRAMEGEN_MASK_MOTION`
  (0-4, default 0.6) how far along the motion karts are looked for.
- HUD detection compares the real frame with the HUD-less one: pixels that differ count as HUD.
  `BB_FRAMEGEN_HUDLESS=0` interpolates the frame with its HUD instead (the HUD then moves with
  the scene), to tell the two apart.

## Diagnostics

| Variable | Effect |
|---|---|
| `BB_FRAMEGEN=1/0` | frame generation on/off (the setting otherwise) |
| `BB_FRAMEGEN_HUDLESS=0` | no HUD-less frame: interpolate the frame with its HUD |
| `BB_FRAMEGEN_MASK=0/2` | karts interpolated too (ghosting) / the real-frame area shown in magenta |
| `BB_FRAMEGEN_MASK_RADIUS=N` | margin around the karts taken from the real frame (render pixels, default 3) |
| `BB_FRAMEGEN_ERROR=N` | reprojection difference (0-1, default 0.12) above which a pixel joins the karts' mask |
| `BB_FRAMEGEN_MASK_MOTION=N` | how far along each pixel's motion karts are looked for (frames, default 0.6) |

The log says when the interpolation starts (`Frame generation: render WxH, display WxH`) and why
it stops (`Frame generation: ...; off for this session`), with the FSR-Vulkan validation issues.

## Status

Built with MSYS2 CLANG64 and played on Windows 11 with an RTX 3070 (144 Hz screen): the
interpolated frames show and the motion is smoother. The karts' mask needs more testing on other
tracks and GPUs. Not tested on Linux, the Steam Deck, AMD or Intel GPUs.
