# 30 FPS reference audit

The reference is the original PS4 behavior at 30 FPS. Higher refresh rates must
not silently change the rate of a simulation response. Physics has priority;
the audit also covers replay, input, animation, UI, audio and rendering.

This audit is **not complete**. Offline tests do not establish that grass cuts,
Polswid, pipes, ground adhesion or every track behaves like the 30 FPS game.
The game has not been launched during this work.

## Changes with offline regression coverage

- Native 30 FPS detection: preserve both 273/8192 and 274/8192 second frames
  produced by the original clock. The former incorrectly activated corrections
  on 28 of 30 nominal reference frames. All physics, camera and VFX timestep
  guards now share the corrected boundary. CPU execution of the original USA/EU
  clock and native fallback paths covers these quantized values.
- Kart vertical position limit: use the current timestep instead of multiplying
  the separation by 30 above 30 FPS. Base and surface lateral retention use
  their equivalent 30 FPS decay. Native bridges preserve live registers and
  capture timestep before the original stack slot is reused.
  Position-limit correction applies only when simulation and raw frame time
  agree; scaled-world contact limits stay native pending a proven response.
- Lateral overspeed response: include the native normalized-speed divisor in
  the reference lateral factor. It previously remained a per-update loss after
  retention was corrected. Supported grounded motion uses the same reference
  intervals as longitudinal speed, including quantized-clock boundaries. Other
  native motion paths normalize the combined divisor and retention locally.
  The ground-speed cache now measures projection roundoff against the original
  velocity magnitude, so cancellation near zero forward speed does not reset it.
- Freeflight position: correct the gravity-dependent integration bias only when
  all supported contact and constraint solvers leave the motion untouched.
  Markers include zero-impulse contacts. Unknown solvers/schemas, missing records,
  table saturation, changed forces, damping/stabilization and multiple microsteps
  retain native integration. The bounded registry supports worker threads and
  excludes static motion zero. Native30 integration and stored velocity remain native.
- Steering-wheel animation correction is disabled. Offline tests matched the
  30 FPS smoothing response, but gameplay validation found visible stutter;
  production retains the native wheel blend.
- Physical steering: preserve the native 30 FPS nonlinear angular-acceleration
  response when the original acceleration multiplier is one. The correction
  takes fractional iterations of the native error map, including its transition
  from geometric decay to a constant acceleration limit. Native 30 FPS paths
  and other multipliers remain unchanged. The complete angular controller
  `0xe454d0` runs against eight original driving archetypes, three driving states,
  five starting angular velocities, six simulation scales, three surface
  modifiers, changing steering and two headings. USA/EU each pass 30,240 cases,
  including 4,320 bit-identical 30 FPS trajectories. Worst angular-velocity gap
  falls from 195.218140 to 0.001832 degrees/s across the initial half-second
  trajectories. Additional 72/144 FPS runs last 2.5 seconds and remain below
  0.004014 degrees/s; errors above 0.003 must be bit-identical to the unmodified
  native result, isolating retained float accumulation in linear-only paths.
  This checks angular velocity, not complete contact-driven kart trajectories.
- Native quaternion integration: scale only the temporary angular velocity used
  by the final integrator, preserving the normalized Euler response at 30 FPS.
  The original stored angular velocity and linear integration remain unchanged.
  USA/EU CPU tests cover 1,056 trajectories per region, including changing axes,
  four initial orientations, mixed cadence and four simulation time scales.
  Worst angular error after one second falls from 75.019010 to 0.000513 degrees.
  All 176 reference trajectories
  stay bit-identical at 30 FPS; invalid timesteps retain native instructions.
  The generated bridge tests preserve all general registers and full YMM lanes.
  Constant-axis trajectories also cover 72 and 144 FPS.
- Camera height, planar position and pitch spring correction is disabled in
  production after gameplay validation found visible shake and stutter. The
  offline fixture retains per-channel 30 FPS
  spring anchors and interpolate between their updates. Original 30 FPS and invalid-input paths still
  execute the original instructions. Generated bridges run against original
  USA/EU code with all eight observed map damping values, negative damping,
  changing targets and cadence, four simulation time scales, and 72/144 FPS
  constant targets. Across 17,280
  boundaries per region, position error stays below 0.000290 units and velocity
  error below 0.003907, compared with a native position gap of 52.1209 units.
  Moving-target USA/EU probes cover 34,560 boundaries per region at 60–240 FPS,
  eight damping values and three simulation scales. Maximum position and velocity
  gaps are 0.000008 and 0.000321. At normal time scale, the largest change in
  frame displacement is 2.316 at a 30 FPS boundary versus 2.283 between boundaries.
- Camera direction spring correction is also disabled in production. Its
  offline fixture accumulates a 30 FPS reference phase, interpolates
  unit-direction targets, and apply each crossed reference step on slower frames.
  Preserve the native spring at 30 FPS; use native fallback on discontinuities
  and near wrapped-angle boundaries. USA/EU probes match 688 moving/static
  target boundaries across 20–240 FPS, with maximum position gap 1.79e-7 and
  velocity gap 3.34e-6. A mixed 24–240 FPS cadence matches 25 additional
  boundaries, with maximum position gap 2.09e-6 and velocity gap 1.24e-5.
  Full camera/gameplay trajectories remain unverified.
- Unboosted overspeed during acceleration/braking: preserve the native 30 FPS
  decay ratio above 1.05 times effective top speed. This applies only without
  steering-loss penalties and with a speed multiplier of at least one. Actual
  USA/EU native code and 24 real curve datasets verify the corrected branch;
  the 30 FPS result stays identical. Active boost, slow surfaces and natural
  deceleration are excluded from this overspeed correction.
  Normalization uses raw engine frame cadence, separately from simulation time
  and scaled curve time supplied by native callers. Native regressions caught
  earlier assumptions changing 30 FPS output when either time was reduced.
  Tests cover four simulation scales and four independent curve-time scales
  at 30–240 FPS, with exact native 30 FPS results. Missing or invalid frame
  context retains native code.
- Network item lead: use elapsed time for speed smoothing. The estimate feeds
  visual lead calibration; network teleport tolerance remains unchanged.
- Replay: nickname retries, ghost-look maintenance and long startup delays keep
  their measured 30 FPS cadence. Short lifecycle settling remains frame-based.
  Recorded item/damage windows and speed history use timestamps instead of
  assuming every recording has 60 samples per second. Window durations preserve
  the behavior of a 30 FPS recording, including where old comments assumed 60.
- FPS readout: preserve the 30 FPS smoothing response using present timestamps.
- Rebuilt-velocity VFX motion: normalize the drag step for the exact supported
  operator sequence represented by the original podium smoke asset. Runtime
  checks require all 15 operator types, enabled flags, semantic fields, compiled
  motion layout, a normal worker context and a separate animation stream slot.
  Prewarm and unsupported sequences retain native arithmetic. USA/EU each pass
  192 native cases, including 32 identical 30 FPS cases and four simulation and
  primitive time scales. Committed-position error stays below 0.000153 units.
  Tests run native tail math and verify it cannot overwrite committed motion;
  they never execute sprite submission or a graphics API.

- Ordinary acceleration: retain native ten-iteration curve inversion once per
  reference interval and interpolate its endpoints at higher FPS. The native
  clock uses 273/274 ticks per reference interval; fixed-clock override uses
  1/30 second. Actual curve-time scaling is retained. This correction requires
  gas without brake, nonnegative speed at or below the effective speed limit,
  no boost, no active damage curve override and no steering-loss penalty.
  Per-kart state resets on map loading, skipped/repeated frames, identity,
  input, curve or parameter changes, and external speed changes beyond float
  projection roundoff. Native 30 FPS still executes unchanged.

- Grounded coasting/braking: interpolate the native speed curve together with
  its signed 50-unit stopping decision. The hook runs before native speed
  storage/projection, preserving forward and reverse stopping boundaries.
  It requires gas off, native state zero, control flag +36 clear, no active
  boost/damage override or steering-loss penalty, and zero surface longitudinal
  blending. Surface +28 retains its native brake-to-coast curve selection.
  Existing AI speed tuning is carried through the motion call; shared archetypes
  are never modified. This remains the fallback for the broader grounded map below.
  USA/EU stopping tests cover starts at and around +/-50, both directions,
  1–60 reference intervals and changing cadence.
- Grounded combined controls and surfaces: evaluate the native gas/brake/control
  selection, combined-control speed limit, signed stopping boundary and longitudinal
  surface blend together at the reference cadence. Interpolate selected base speed
  before its native stores at `e46a55`, then apply the blended speed before native
  vector projection at `e46acf`. Normalized speed still follows the native base-speed
  calculation. The scope excludes airborne/state-two motion, changing boost, damage
  overrides, steering-loss penalties and the native damage slowdown floor. Input,
  surface, identity, time-scale or external impulse changes restart the interval.
  This preserves the native surface formula rather than exponentiating its weight.
- Steady active boost: extend the grounded reference map with native boost addition,
  surface blending and floor/cap ordering. The stored boost amount must match the
  current amount; onset, fade and malformed inputs retain the previous correction
  path. A changed boost cap or floor also retains that path for the current update,
  preserving the native immediate clamp. The original instruction contracts verify
  boost loads, subtraction, normalization and clamp order, including floor above cap.
  This does not establish equivalence during arbitrary boost transitions.
- Fractional-scale integer timers: distribute each native 273/274-tick interval's
  integer budget across high-FPS updates before the native clock addition.
  Normal counter-clock totals match native30 at steady scale. Fullspeed,
  native30, fixed override and the float simulation timestep remain unchanged.
  Scale/pause/reset discontinuities start a new interval without correcting past
  time. This fixes steady fractional-scale drift; it does not resample controls
  or make arbitrary mid-interval changes identical to native30.

## Physics investigation still open

## Full 30 FPS scheduling investigation

An offline USA/EU fixture executes the original main loop with external
gameplay, world, network and graphics calls stubbed. A candidate gate retains
native 30 Hz simulation ticks while rendering at 30, 60, 144, 240 Hz or mixed
cadence. It preserves the tested native clock traces, pause/scale/reset cases,
paired async dispatch/join, and presents a private CRender clock each render.
This is a design proof only: no production scheduler or interpolation hooks are
installed. In particular, the candidate does not implement the live timer
source, slow-frame catch-up policy, or all presentation consumers.

Native camera output, root transforms and pooled skin palettes also have
isolated CPU hook fixtures. Camera tests cover four cameras, split-screen
aspect, reset/discontinuity and nested pre-render ownership. These hooks remain
scratch code; they do not run a complete scene traversal or renderer. Particle
presentation and remaining native animation still need their own consumer and
lifecycle contracts before a full scheduler can be installed safely.

Native speed calculation `0xe4a220` inverts an acceleration/deceleration curve
through `0xa98cc0`, passing an iteration limit of 10 at `0xe4a3cb` and `0xe4a3ef`.
Repeated inversion introduces quantization that accumulates differently with
update rate. Offline execution of the original function with all 24 linear
velocity datasets from `update2.pak` reproduces this with real curve bytes.

For one dataset, speed starts at 3637.5 units/s and reaches 3132.73 at 30 FPS,
but 2420.21 at 240 FPS after one second. Raising only the offline search limit
to 24 largely removes that difference, but also changes the 30 FPS reference.
The precision increase is therefore **not applied to production**. A second candidate based on
evaluating a 30 FPS target still diverges in other cases and was rejected.
The fixture omits a boost component and does not reproduce a complete race.

The narrower unboosted overspeed branch is multiplicative. Its native 30 FPS
ratio can be normalized without replacing the inverse search or changing the
30 FPS result. This correction is implemented. Ordinary acceleration is now corrected separately. Quantization in other
speed regimes is still open; the overspeed test does not establish whole-race
equivalence or cover crossings into another speed regime.

The longitudinal surface blend must not be blindly exponentiated: its candidate
already incorporates timestep. Grounded fields `+0x214/+0x215` are current and
previous state, not evidence for a new 33 ms timer. The gate at `0xe468af` uses
`(global +0x20 - kart +0x194) / 8192`; it must not be treated as a render counter.

## Coverage boundaries

Another offline candidate weighted the difference between precise inversion
and native ten-iteration inversion by `dt * 30`. It preserved all 30 FPS
samples but still left 84 of 432 cases more than 10 speed units apart at
240 FPS, with a worst difference of 114.274 units. Twelve cases became
worse by more than one unit. This candidate was rejected and is not part
of the production patch.

A separate native probe finds plateaus in all 72 speed maps (24 datasets and
three speed kinds). An anchored 30 Hz candidate matches 2,592 isolated
trajectories, but is not installed: the real motion caller recomputes speed
from velocity projection and can invoke multiple speed kinds in one frame.
One cached state per kart cannot safely infer those transitions from the
previous returned speed alone. The complete linear-motion fixture below now
checks the surrounding native branches, without simulating a complete race.
The same candidate fails 176 of 288 scaled-curve-time cases, reverses the
requested speed change in 40 tested steps, and resets on every update under
one-ULP input perturbations. Its isolated endpoint result is not a safe fix.

The complete native linear-motion caller `0xe46250` now has a CPU regression
using the relocated original driving-archetype graph. It supplies a fixture boost
component lookup, a no-op entity-change notification and host `sinf/cosf`;
the original ELF import relocations verify the math names before execution.
The linear fixture runs actual projection, speed selection and velocity writeback.
It does not run grounding, contacts or a full drift. A separate angular fixture in
the same executable covers the steering correction described above. It initializes
the native math globals and supplies a valid quaternion and unit transform scale.

For the first archetype, unmodified straight acceleration from rest reaches
1631.57629 units/s at 30 FPS but 1953.38464 at 240 FPS after one second.
The new ordinary-acceleration correction fixes this path.
The regression covers eight archetypes, seven input/start states, eleven
orientation/modifier/AI/surface scenarios, four simulation scales, and
30/32/60/64/120/128/144/240/256 FPS plus mixed cadence. It executes the real
native clock in both counter and fixed-override modes: 49,280 cases and 4,928
bit-identical 30 FPS results per region. Ordinary acceleration from rest stays
within 0.000245 units/s of the reference; all tested grounded scalar results,
including overspeed, stay within 0.003175 units/s. Eligible coasting/braking/reverse
cases stay within 0.001161 units/s;
16,000 additional stopping-boundary cases per region stay below 0.000550 units/s.
Another 21,120 cases exercise every gas/brake/control combination, eleven starting
speeds, eight archetypes, ordinary and special surfaces, five cadences and both
clock modes. Their velocity, base-speed and normalized-speed results stay within
0.002198, with 4,224 bit-identical native30 controls. The broader offline matrix
contains 619,520 cases and 61,952 exact30 controls, with maximum gap 0.004395.
The fixture also installs the actual lateral bridges and the production dt capture.
Its 11,520 sideways cases include overspeed, near-zero forward projection, combined
controls, surface retention and one/30 reference-frame durations. They remain within
0.002198, with 2,304 exact30 controls. Removing either the overspeed-divisor correction
or the projection-roundoff bound fails this regression. An expanded one-frame matrix
passes 168,960 cases with maximum gap 0.000489 and 33,792 exact30 controls.
Steady boost adds 19,200 cases and 3,840 exact30 controls per region, covering
all eight archetypes, combined controls, surface blends, both clock modes and
floor-above-cap ordering. Maximum speed-norm gap is 0.000677 units/s, base-speed
gap 0.000245 units/s and normalized-speed gap zero. Direction differs by at most
0.000003753 radians: repeated native braking normalization and projection retain
float roundoff, producing an axis-component gap of 0.011231 units/s in one case.
This direction residual remains open as issue #0041; scalar agreement does not
resolve it. Another 160 cases preserve immediate cap-drop and floor-rise clamps.
The clock bridge passes 396 native steady cases and 24,000 scale/pause/reset
frames per region, preserving all native clock fields except corrected integer
simulation ticks. The new regression fails against the pre-acceleration source.
A separate C regression checks changing cadence, cache capacity, input changes,
identity changes, skipped/repeated frames, map epoch, damage/boost exclusion and
external speed discontinuities.

Unlike the earlier rejected anchor, the installed correction accumulates raw
frame time, accepts projection roundoff, uses stable curve identity instead of
a temporary AI data pointer, and interpolates endpoints rather than repeatedly
inverting partial curve time. The quantized native clock exposed a 6.133667
units/s error in the first version despite ideal-timestep tests passing;
matching its 273/274-tick intervals removed that discrepancy. Fixed override
is detected with the same 5e-7 threshold as native code.

Input or parameter changes start a fresh reference interval. Exact decisions
for arbitrary changes inside that interval, active damage,
boost onset/fade and the complete collision/solver trajectory remain outside
this fix. Scalar agreement does not establish complete drift/contact equivalence.

An offline candidate applied native ten-iteration inverse rounding only once
per 30 Hz interval, using a precise inverse for intervening substeps. It passed
8,064 bit-identical callback controls and reduced the straight acceleration,
coasting and drift-state endpoint gaps below 0.009 units in the initial
1,344-case motion matrix. It is **not installed**: scalar overspeed still
diverged, and expanded motion tests worsened some steering/slowdown cases.

Tracing the complete caller also exposed a branch-timing difference that a
curve-only correction cannot solve. Native motion compares projected speed
against +50/-50 units at `0xe466f9 / 0xe46990`. In one braking fixture, the
30 FPS final step starts at 82.62476 and ends at 30.28729 units/s. At 120 FPS,
the candidate reaches 43.23318 before the final substep and the caller stops
the kart immediately, yielding zero at the same elapsed time. The reference
does not stop until the next 30 FPS update. A safe solution must preserve
decision timing as well as curve response. The new grounded gas-off correction
includes that threshold in its reference map and changes only the selected
scalar speed. The broader grounded map now covers combined controls too;
general physics scheduling remains open.

The native dispatch at `0xe252a0` calls grounding (`0xe2f860`), speed
modifiers (`0xe452c0`), angular motion (`0xe454d0`) and linear motion
(`0xe46250`) in sequence. The following state update (`0xe47e70`) advances
timers using the message timestep. The gravity-direction update (`0xe48540`)
passes its vector to `0xa7dd50`, which overwrites a registered vector rather
than accumulating an impulse. These observations do not establish contact
solver cadence or correct adhesion across changing ground normals.

The gravity registry callback at `0xa32c10` multiplies its stored vector by
the magnitude of the incoming gravity vector. Registration at `0xa82590`
connects this callback through vtable `0x254cd30`. This is not evidence of
an extra impulse per rendered frame. In the input-state update, `0xe2fb6f`
advances the direction-hold timer with the message timestep; neighboring
jump/slide timing also reads the native timer rather than counting renders.

The world update at `0xa1a780` clamps its source timestep to `[0, 0.1]`
seconds and passes it to pre-motion callbacks. Its call to `0x506650` is
identified by the native `hkbWorld::stepMultithreaded` profiling string as
the behavior/animation update. Its stage loop is not evidence of contact
solver substeps. No world-step or grounding-timer patch is justified by
these observations alone.

The native speed regression also exercises boost, slow-surface,
steering-loss and natural-deceleration branches using original USA/EU code.
Across 24 curve datasets, three speed kinds, four modifier states and seven
starting speeds, 30 FPS outputs remain exactly native. Paths outside the
scoped overspeed correction remain exactly native at higher rates. Mixed
240/80/60 Hz updates also match the tested 30 FPS overspeed decay within
0.01% of top speed. This proves branch isolation, not equality of the
remaining native speed regimes across frame rates.

The controller and solver share the main frame loop, not a physics-only callback.
Controller dispatch is `9f4b2f / c3c250 / 997330 / bc2790 / bc29c0 / caa590 /
ce3e20 / e252a0`; the payload comes from `entity+0x1a0`, and its `+0x18` timestep
comes from clock `+0x24`. Physics world update occurs later at `9f4bae / a1a780`.
Messenger, input, interaction, GUI and AI updates lie between these calls.
The world enters `506650 / 507740 / 506dc0 / 5409e0`, then scene modifiers
`540580 / 540410` call physics interface `4edf30`. A context flag prevents a
second solver step. Async phase dispatch and join must remain paired.
The inspected `CRigidbodyInterpolationBuffer` is network history, not a ready
local render interpolation implementation. A standalone world-step gate would
leave the controller and clocks running at a different cadence, so none is installed.

The contact path is now traced separately: `0x4edf30` puts its incoming
timestep into the step descriptor passed to `0x6d6030`. Collision preparation
continues through `0x6d5ac0` and `0x6d5390`; the call at `0x6d584e` passes that
timestep to the solver setup function `0x6e7af0`. This establishes a path
inside the native physics library, connected to the frame dispatcher above.

Isolated execution of the original USA/EU solver setup verifies that it stores
`dt`, `1/dt`, `dt/iterations`, `gravity*dt` and `gravity*dt/iterations`.
The regression covers 162 combinations of gravity vectors, iteration counts,
microstep counts and update schedules, including mixed 240/80/60 Hz steps.
Both gravity increments accumulate to the same one-second value within
0.0001 units at 30, 60, 90, 120 and 240 FPS. This rules out a missing timestep
factor in these setup fields. It does not execute contact resolution,
integration, friction or changing support normals, so those remain open.
No production solver or gravity change follows from this result.

Position integration has a separate demonstrated difference. The native world
selects solver preset 5 through `0xa82590 / 0x6b53a0`: four iterations, one
microstep, tau 0.6 and damping 1. Isolated original USA/EU execution through
`6e7a20 / 6e7af0 / 80ec70 / 878840 / 878bb0` averages substep velocities but
retains an acceleration-dependent position bias. With gravity -980 and no
contacts, after one second the Y position is -494.083038 at 30 FPS and
-490.509949 at 240 FPS, while velocity is approximately -980 in both cases.
There are 240 original probe cases per region; this is not a complete race.

A candidate based on total solved velocity change fixes constant-acceleration
drift but fails an impulse counterexample: native Y stays approximately 200
at both rates, while the candidate produces 200.728882 at 240 FPS. The 96-case
candidate probe includes scaled time, changing cadence, impulses and a projected
grounded case. This candidate is rejected. Native motion reflection and the
final integrator's direct buffers expose no proven contact-free marker; a safe
gate was required before correcting freeflight alone. The installed scoped
correction below uses explicit solver-participation markers instead.

The installed freeflight correction instead captures the pure gravity increment
before `80edf7`, checks subsequent increments at `878b3a`, and tracks participation
in scalar contacts (`81ae17`), optimized contacts (`8c37f0`) and generic constraints
(`854483`). Only untouched eligible motions adjust mean integration velocity at
`8790b7`; stored physical velocity and impulses remain native. USA/EU each pass
3,360 motion/contact cases, 31 fallback/lifecycle/concurrency controls, all four
scalar dispatches on both sides, and 24 quantized-clock trajectories. The 689
signature mutations are rejected and the 3,072-byte bridge guard remains intact.
The one-second maximum gap is 0.00167847 units. Ten-second tests retain a separate
native floating-point velocity accumulation difference: at scale two and 240 Hz,
the position residual is 6.828125 units. A per-frame double-precision decomposition
attributes this to native velocity history and mean/position rounding, with every
eligible correction applied. Common simulation scheduling remains necessary for
exact long-run equivalence; no duration-based cutoff is installed.

The final integrator at `0x8790eb` advances orientation by
`normalize(q + 0.5 * dt * omega * q)`. Its per-step angle is
`2 * atan(length(omega) * dt / 2)`, which changes the accumulated rotation with
FPS even for constant angular velocity. Let `steps = raw_frame_dt * 30` and
`reference_dt = simulation_dt / steps`. Above 30 FPS, the correction multiplies
temporary omega by `tan(steps * atan(length * reference_dt / 2)) /
(length * simulation_dt / 2)`.
The original 17 bytes are validated in both regions before hooking.
The kart applicability trace covers all 21 kart assets per region and the path
`f05240 / e965d0 / e96fb0 / a79d90` into the physics angular-velocity field.
These trajectories do not establish equivalence during arbitrary contact or
input changes between reference-frame boundaries.

Native time update `0xa31a10` stores raw/fixed engine timestep at clock `+0x18`
and applies `ANIMATION/AnimationSpeed` to produce the simulation timestep at
`+0x24`, after clamping raw time to `[0.001, 0.1]` seconds. The tested 30–240
FPS interval does not hit that clamp. A small simulation timestep alone
therefore does not prove high FPS.
All frame-normalization guards now use raw engine cadence. Rotation and camera
retain the simulation/raw ratio for the 30 FPS reference step. Scaled-time
regressions fail with the earlier guards and pass with this separation.
The outer physics dispatch and cross-thread clock lifetime have not been
fully reproduced by the isolated native fixtures.

The normal clock source quantizes a cumulative monotonic counter before taking
frame differences. At scale one, the original USA/EU code accumulates exactly
81,920 ticks in ten seconds at all tested rates; a fixed-dt override can create
an artificial drift and is not an adequate clock fixture. Fractional simulation
scales do expose per-frame truncation: scale 0.25 produces 20,400 ticks at 30 FPS
but 19,200 at 240 FPS over ten seconds. The scoped clock fix now produces
20,400 ticks at both rates, also handling rates such as 128 FPS where native
truncation originally loses fewer ticks than native30. On scale or pause changes,
it abandons the old partial interval and anchors the new budget at the current
frame; time never goes backwards. Exact equivalence to controls first sampled
at a different native30 boundary still requires a common simulation/input
schedule and remains open under #0029. Fixed overrides stay native because their
fullspeed rounding differs from the normal cumulative counter.

All 21 kart assets per region select `PhysicsMotionPropertiesPresets.BaseKart`.
Its linear and angular damping are both zero; native `0xa84d50` copies those
values into the Havok damping fields. Thus the linear retention calculation at
`0x878eab` is one for these karts and does not justify another kart damping fix.

The direction spring at `0xa997c0` wraps component errors to `[-pi, pi]`.
Its scoped correction falls back to native behavior when target samples jump or
the wrapped error approaches a branch boundary. This avoids claiming equivalence
for cuts or arbitrary camera inputs. The camera corrections do not prove that
the entire camera path is independent of FPS.

The VFX drag operator at `0x2c4d30` scales its contribution by effect time.
An isolated feedback-velocity probe supports a normalized decay, but actual
smoke stacks load velocity from the attached object before applying drag.
That changes the recurrence, so the general decay candidate is not installed.
The supported rebuilt-velocity sequence instead uses its equivalent 30 FPS
effect timestep only for drag. Its context timestep remains unchanged.
Normal worker `0x2b3670` supplies a scoped thread-local manager; the math hook
at `0x2c4f98` requires that manager and a non-null worker context. Prewarm uses
a separate fixed step and is excluded. Native tests also verify nested worker
restoration, GPR/YMM preservation and 41 rejected contracts. Range/curve drag,
other sequences, scheduler/lifecycle behavior, rendering and arbitrary input
sampling remain open; this is not a fix for every particle effect.

- Input calibration, audio, presence and network workers already use their own
  clocks or sample periods. No new defect was demonstrated there.
- ImGui, DLSS and FSR receive elapsed time. TAA history, jitter and motion vectors
  are defined between samples; changing their constants needs separate evidence.
- A native scan found 106 RIP-relative references to float 30, 60, 1/30 and 1/60
  across the executable segment. Constants are investigation leads, not bug
  findings. This does not cover every implicit per-update operation.
- Native animation, wrapped camera direction, particles, projectile motion and the remaining
  physics paths still require deeper evidence and gameplay verification.

## Runnable checks

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -Isrc tests/test_race_options.c src/runtime_tt_reference.c src/runtime_ai_catchup.c -lm -o /tmp/test-race-options
/tmp/test-race-options
cc -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Isrc tests/test_item_lead_timestep.c -Wl,--gc-sections -lm -o /tmp/test-item-lead
/tmp/test-item-lead
cc -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Isrc tests/test_replay_timestep.c -Wl,--gc-sections -lm -pthread -o /tmp/test-replay
/tmp/test-replay
python3 -m unittest discover -s tests -p test_fps_overlay.py
BB_TEST_GAME=/path/to/CUSA13795-1.21 PYTHONPATH=scripts python3 -m unittest discover -s tests -p test_kart_timestep_native.py
BB_TEST_GAME=/path/to/CUSA13795-1.21 PYTHONPATH=scripts python3 -m unittest discover -s tests -p test_freeflight_timestep_native.py
BB_TEST_GAME=/path/to/CUSA13795-1.21 PYTHONPATH=scripts python3 -m unittest discover -s tests -p test_vfx_timestep_native.py
# Repeat the native instruction contracts with CUSA14876-1.21.
```

The native suite reads the dump and executes isolated speed/curve, quaternion
integration, camera spring, selected VFX operators and solver setup code on the
CPU with test data. It does not start the game,
initialize a graphics API or access the GPU.
