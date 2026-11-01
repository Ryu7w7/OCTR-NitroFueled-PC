# Extended AI difficulty research

## Scope (2026-11-01; corrected by user)

User requests stronger selectable difficulties and driving/item tuning in Arcade,
Private Match and Public Matchmaking, excluding Adventure entirely,
with more aggressive item use, better racing performance and less reliance
on rubberbanding. Initial proposed labels: Very Hard, Extreme, Nightmare.
A community report claims forcing difficulty 3 gives faster AI and more
Clock/Orb use but breaks Adventure bosses. Treat that report as an unverified
lead, not a safe implementation recipe. Do not run or control the game;
user owns live tests. Build executable only, never GPU.

## Native entry points found

- CGuiBehaviorRaceDifficultySelector: vtable 2770d18, event 125fb30 (USA).
- CGuiBehaviorAiDifficultySettingSelector: vtable 2770e38, event 1260240.
- CDifficultyManager metadata registration 18525da.
- Driver difficulty class table, difficulty-specific TTR and power-up weights
  are separate native data types. A label or raw enum change alone is not
  sufficient evidence of improved racing lines or safe boss behavior.

Implementation and parameter mapping are in progress. No new AI profile has
yet been validated or included in the executable.

## Findings

Native EDifficultyLevel contains Invalid, Easy, Medium, Hard, Nightmare, Count.
The active race selector 125fb30 populates three difficulty labels, with an
optional No AI entry. The AiDifficultySettingSelector is a stub that displays
"not implemented"; it is not the active online selector.

125ffc0 subtracts the optional No AI index and calls c0a3e0. c0a3b0 returns
manager +0xc for the authority or +0x10 for a peer, chosen through 3ee590.
The setter updates save bookkeeping and publishes the host difficulty; new
values must not be pushed through that path without auditing its bounds.

update.pak loosedata/kartaisystemdata.igz contains actual racing and Battle
parameters: target range/cone/lock delay, attack delay, boost success/failure,
maximum velocity, hurt rubberbanding, and position-dependent item weights.
Many serialized fourth difficulty values are zero, so blindly selecting native
Nightmare is not an acceptable substitute for tuned profiles.


## Updated user scope and implementation in progress

Adventure is excluded (including bosses); extended difficulties belong only to
Arcade/Online/Private/Public. All eligible modes must receive the tuning. Itemless Private Single AND CUP
are explicitly included, with actual driving improvements across
Easy/Medium/Hard/Very Hard/Extreme/Nightmare. Item aggression
alone is not a sufficient Itemless implementation. User requests drift lines,
U-turns, hops and shortcuts resembling Time Trial driving. These capabilities
must be verified independently, not claimed from numeric difficulty changes.

Current unbuilt runtime_race_options.c adds three labels to the active native
selector and maps them to supported Hard (2), refining actual AI global values.
The native fourth column is not used. Initial parameter changes include target
lock time, Battle attack interval, start boost consistency, hazard avoidance and
hurt catch-up cap. Profile selection is currently process-local; native host
still replicates Hard. Host authority/profile display sync requires further work.

Driving findings: CKartAIRaceControllerComponent vtable 2583c60, update dce820;
flags +50/+51 are canDrift/canDriftBoost. Native dcf1c0 issues hop/drift inputs,
dcf740 uses jump-boost intent from AI component +7d. Drift continuation uses
native geometry/grounding gates dcfb10/dcfc10, which must remain intact.
CKartAIRaceTacticalComponent vtable 2583e50 update dd0210 reads spline lookahead
and learned segment data. dd1520 selects stats by track hash and segment ID.
No implementation of Time Trial ghost playback, U-turn technique, or all-track
shortcut selection has been established yet. Do not label boost tuning as these.

Implemented: Adventure exclusion in selector and active tuning, and all six levels
in Arcade and Online (including Private/Public and Itemless). Driving-route audit
and host profile replication remain incomplete; do not claim these are implemented.

Scope clarification: tuning is NOT private-only and NOT Itemless-only. The
Itemless predicate must never gate the shared driving improvements. Adventure
is the only excluded game category. Public matchmaking remains governed by the
host/server for difficulty selection; local UI changes cannot control a remote
authority. Profile replication must be assessed before claiming parity.


Verified Adventure boundary: CClient singleton 2ab3cd8, gameMode +144,
nextGameMode +148 (metadata 181a34a, names table 25e2840, offsets 1d9fa70).
EGameMode values are Story=1, Arcade=8, OnlinePlay=16. The runtime applies shared
AI tuning only to 8/16 and restores original Easy/Medium/Hard columns otherwise.
The active Adventure difficulty selector is the only scanned selector with
includeNoAI +1b9 false, so it retains the native three choices. Arcade race,
Battle, and private settings use includeNoAI true and receive extended labels.

Additional requirement: Itemless high difficulties should manage boost reserves
and retain Sacred/Blue Fire through better drift boost decisions, not unconditional
fire locking. Native start boost assignment e9de80 and drift decision dcf1c0 were traced
separately; the earlier dc8310/dc8a70/dc8bd0/dc9100 lead did not establish drift timing. Speed, reserve duration, wall penalties,
and unlimited-boost flags have not been changed.


## Reserve-aware drift boost implementation (2026-11-01)

`runtime_race_options.c` now wraps f4c840 (EU f4c830) and only adjusts calls
from the AI drift controller at dcf477 (return dcf47c in both regions).
Human input and Adventure calls retain the original threshold. All native
speed, drift state, timer-active and maximum-consecutive-boost checks execute.
Arcade and Online share this path, including Private Itemless Single/CUP;
there is deliberately no Private-only or Itemless-only gate on this improvement.

Thresholds as fractions of the native drift timer are .80/.84/.88/.92/.95/.97
for Easy/Medium/Hard/Very Hard/Extreme/Nightmare. Fuller boosts should bank
more reserve; this is a tuning hypothesis awaiting the user's track tests,
not a measured lap-time or guaranteed perpetual-fire claim.

When the fastest active fire has less than 1 second remaining, the controller
uses the original .80 threshold instead of waiting for a fuller boost. Reserve
reading follows native e2e6b0's carry formula: e2f050(boost) + data[1c] +
boost[28] - boost[18]. CKartBoost metadata 1907b3a confirms these fields.
The reader chooses the highest boost-data speed, not the longest lower-speed
reserve, so a long red-fire reserve cannot hide expiring blue fire. Maintained
or permanent boosts have no immediate expiry. Blending-out boosts are ignored.
No reserve, boost speed, boost duration, cancellation flag or collision penalty
is written. Walls and failed driving can still lose fire.

Correction: AISystemData +80/+88/+90 concern **START boost**, not consecutive
drift boosts. e9de80 assigns starting intent to AIComponent+80; dcfa00 consumes
it against the race-start timer. The maximum counts eligible AI racers. The
native comparison uses RNG in [0,1), so runtime chance values are fractions,
not serialized 25/40/70 percentage magnitudes. Start eligible counts now
2/3/5/6/7/8, chances .40/.60/.80/.87/.93/.97. These values do not implement
U-turns, shortcuts or new racing lines.

Validation: C unit checks cover all six levels in Arcade/Online, native failure
propagation, human/Adventure exclusion, fastest-fire expiry and no reserve
refill. Signature and call-target checks protect the USA/EU hook installation.
No game process was launched or controlled.

Remaining substantive limits: profile choice is process-local (native wire/save
still carries Hard for extended profiles); guest labels do not yet replicate
Very Hard/Extreme/Nightmare. Track-specific U-turns/shortcuts and learned-line
changes are not implemented. These must not be described as completed.


## Racing-line / shortcut trace in progress

Actual Crash Cove RaceSystem in update2.pak contains 25 splines, including
AI_Spline_jump_split_01/02, 6 CRaceTrackSplineComponentData records,
4 CRaceTrackSplineTransitionComponentData and 3 CRaceTrackForkCheckerComponentData.
This provides a concrete native branch/transition path to investigate instead
of inventing unconstrained corner-cutting. No transition policy has been
modified yet. dd0210 samples separate drive and drift lookahead positions from
the active spline, incorporating per-segment stats and current speed.

Executable-only build completed with the reserve/countdown/UI patches on
2026-11-01. GPU was not built. USA and EU asset tests passed (14 each);
reserve/profile and online-mode C unit checks passed. No live gameplay tests.


Confirmed path metadata: CRaceTrackSplineComponentData +10 start, +18 end,
+20 pathDifficulty, +28 respawnPoints, +30 isShortcut, +31 isHazardous.
Transition data holds previousPositionPaths +10, nextPositionPaths +18,
previousAIPaths +20, nextAIPaths +28, incrementLap +30. Runtime transition
component caches nextAISegments +48; the position/checkpoint graph is separate.
Forward crossing e9a060 updates lap/progress and dispatches e0ea70 for AI path
selection. These are identified native mechanisms, not yet an implemented
shortcut preference. Do not bypass checkpoint/fork validation.


## Drift-entry frequency and racing-line preference (2026-11-01)

Two independent native decisions are now wrapped in `runtime_race_options.c`.
`e0f9b0` receives a difficulty-weighted target traversal time only at its AI
caller `e0f25b`. The blend toward the fastest eligible recorded line is
10/20/35/55/80/100 percent for Easy through Nightmare. Hazard exclusions remain
in place. This does not select a different checkpoint branch or manufacture a
shortcut: branch selection and spline connectivity still belong to the game.

`dcfb10` is the AI drift-entry predicate. It requires matching signs between
the incoming angle and controller curvature, curvature magnitude above .15,
sufficient native speed, and incoming angle magnitude above .15. The wrapper
scales only its angle argument by .90/1/1.15/1.35/1.60/1.90. Effective entry
angle thresholds become approximately .167/.150/.130/.111/.094/.079, while
native curvature and speed gates stay unchanged. This lets higher levels
start drift on more eligible entries without writing steering, forcing drift
on straight sections, or bypassing the elevated stuck-recovery speed gate.
It applies to Arcade and Online (Normal/Itemless, Single/CUP); Adventure keeps
the original argument. Native continuation, boost count and grounding are
unchanged. The expected increase in real drift frequency requires user testing;
unit input coverage is not a measured drifts-per-lap result.

Checks: matching USA/EU function prefixes; unit cases cover progressively more
accepted entry angles, opposite direction, straight input, stuck speed gate,
Adventure exclusion, hazard-aware line choice and native faster-target retention.
No game process was launched. Explicit U-turn control, new shortcut branch
selection and hop chaining remain unresolved; these are not claimed complete.

Executable-only build containing drift-entry and line-choice hooks completed at 2026-11-01 07:08 WIB (`out/sacredfire`, 2126544 bytes). GPU library remains at 2026-10-31 20:25. Race-options unit checks and `git diff --check` pass. Live driving remains untested by request.


## Selector navigation regression and revised scope (2026-11-01)

User clarifies HOST owns online difficulty. Guests must not independently choose
an AI profile; native host/server authority remains authoritative. Earlier notes
about guest profile work concern replication/display, not a guest setting UI.
User removes U-turn and hop chaining from the requested scope. Shortcut selection
and drift frequency remain required.

Concrete selector defect: +0x190 is the initialization index; +0x1a0 is the live
cursor. Native 125de70 navigates with +0x1a0 and sends the selected index in the
change event. Our previous wrapper called 125da80 on every change, resetting the
live cursor from +0x190 and replaying cycleToggle initialization. When leaving an
extended difficulty for Hard/Medium/Easy/No AI, +0x190 could still contain the
previous extended selection. Removed that per-change initialization; refresh
only after initially extending labels. Preserve the selected UI index separately
from the temporary supported-Hard value passed to the native settings setter.

Regression checks cover descending and ascending selection, all native levels,
No AI, end-to-start wrap, unchanged event value, no duplicate labels, and exactly
one initial refresh. Unit checks pass; no gameplay test performed.


## Connected alternate-route / shortcut selection implemented (2026-11-01)

**Historical interpretation corrected below:** segment +60 is occupancy, not path difficulty. The newer Nightmare policy supersedes that interpretation.

Native e0efa0 receives a reference to the current transition's connected AI
segments. It prioritizes minimum segment difficulty (+60), then recorded traversal
time from the first record in segment+28. That policy can keep higher difficulties
on the easiest branch even when a faster authored alternate exists.

The new wrapper retains native policy on Easy/Medium and Adventure. Hard,
Very Hard, Extreme and Nightmare may consider segments respectively 1/2/3/4
ordinal difficulty steps above the easiest connected non-hazardous branch.
It only chooses an alternate if its positive finite recorded time is strictly
faster than the easiest branch. Segment+65 hazardous branches are excluded;
missing/invalid records or absence of an eligible faster route fall back to native.

The selected branch is passed to the original chooser through a temporary
single-entry list view. No live segment data, list membership, checkpoint graph,
lap accounting or geometry is mutated. The native function stores original line
records on the AI and returns an original connected segment; it does not retain
the temporary list. Native line choice and transition application still execute.
This enables faster authored branches (including authored shortcut routes) where
the dump provides usable time records; it does not create missing AI shortcut
splines or prove every shortcut drivable. High-speed route success still requires
user track testing. U-turn and hop chaining were explicitly removed from scope.

Regression cases cover difficulty caps, faster-only choice, hazardous/invalid
fallbacks, Arcade/Online versus Adventure, original list preservation, and native
handoff. USA/EU prologue signatures match. No gameplay automation was used.

Final executable-only build: 2026-11-01T07:30:36.170374, 2132160 bytes. Includes selector callback relock fix, difficulty cursor fix and connected alternate-route tuning. CPU unit checks pass; asset checks cover USA/EU including both lobby-root versions. GPU SHA256 unchanged (f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e). No live gameplay tests.


## Authored Nightmare shortcut priority and corrected segment fields (2026-11-01)

Confirmed `CTrackSplineSegment` registration 1a175e8 (size68), metadata
1a17670: field-name table26de7e0 and uint16 offsets1da9870 identify
+60 `_currentUsingRacerCount`, +64 `_isShortcut`, +65 `_isHazardous`.
The previous interpretation of +60 as path difficulty was incorrect. Native
branch selection balances occupancy and then compares the route-time value in
segment+28's first record. Serialized spline component data's path difficulty
is a different field and object.

Nightmare now always prioritizes a connected, non-hazardous authored shortcut
with a valid positive finite route-time record. Occupancy does not exclude it,
and it need not beat an ordinary branch's recorded time. Among multiple usable
shortcuts, the shortest recorded route wins. If no usable shortcut exists,
prefer a faster ordinary branch or retain native fallback. Hard/Very Hard/Extreme
retain 1/2/3 additional-racer occupancy allowances; these are congestion
allowances, not path difficulty tiers. Easy/Medium and Adventure stay native.
No synthetic geometry or checkpoint bypass is introduced. This cannot promise
shortcuts lacking authored AI connections or guarantee successful driving on
all tracks without user testing.

Unit checks cover a crowded/slower shortcut preferred over an ordinary faster
route, multiple shortcuts, hazardous shortcut exclusion, invalid-record
fallback, original list preservation and Arcade/Online versus Adventure.


## AI authority and replicated driving input audit (2026-11-01)

Static native evidence (no game launched):
- `CKartAIComponent` input callback dcb630 checks entity+3b bit0. The owning
  branch calls dcb680 (tactical/controller virtual updates), dcb6d0 (item/input
  decisions), then dcb820 (packs steering and six action bits into +70).
- The other branch calls dcb8e0, which reconstructs steering and actions from
  that +70 object instead of executing the tactical/controller branch.
- +70 is `_kartAIInputs`, type `CKartAIInputs`, confirmed by metadata1908988.
  That field receives the attribute constructed by4530a0; registration17189b6
  identifies it as `igNetReplicateFollowAttribute` (name at1c3df4c).
- Native difficulty c0a3b0 selects the authority value+0c or replicated value+10;
  c0aa50 applies the peer update. Extended profile `ai_profile` remains
  process-local and native difficulty packets still carry Hard for those levels.

Thus native driving has one decision-producing authority and replicated input
consumption on the non-authority side. The new shortcut/drift hooks leave this
branching and input replication intact. This is evidence of native synchronized
AI inputs, not proof of frame-identical trajectories or network reliability.
Complete transport/position reconciliation and authority migration were not
exercised. Do not claim guaranteed zero desync or completed extended profile
replication. Only host should configure difficulty; a guest selector is not a
solution for replicating extended profiles. Live two-client validation remains
reserved to the user.

CPU-only build with authored Nightmare shortcut priority: 2026-11-01T07:37:04.588979. Race-options tests and diff whitespace checks pass. GPU hash remains f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.

## Connected approach planning and varied lanes (2026-11-01)

User reports Nightmare does not use Crash Cove shortcut in Arcade Normal Single,
and AI routes look identical. Latest log confirms Nightmare selection before load;
it does not establish that the route hook executed or that a shortcut succeeded.

Native metadata: CClient gameMode is +144 (Story=1, Arcade=8, Online=16).
CTrackSplineSegment +18 is next transition, +3c/+48 are world start/end positions,
+60 is CURRENT OCCUPANCY, +64 shortcut, +65 hazardous. ea08d0 computes endpoints
from the authored spline and transition planes. CTrackRemains +c is timeToEnd,
+10 its own segment; e9f9b0 builds these records by walking the connected graph.

Crash Cove transition 02 includes jump_split_01/02 among ordinary next AI splines.
The actual shortcut flag is on AI_Spline_19 after transition 03. The prior patch
only prioritized already-flagged branches, without selecting the jump approach.
Nightmare now looks one connected transition ahead and prefers the candidate
whose endpoint best joins the next valid non-hazardous shortcut start. Direct
shortcuts remain higher priority. It does not fabricate splines, skip checkpoints,
teleport, add hopping or change human steering. This geometric planning is not
live proof of correct jump timing; that remains a user-test requirement.

For all six levels, comparable ordinary lanes are eligible within a bounded
remaining-time allowance (20/16/12/10/8/5 percent). Current occupancy wins, then
an AI/transition-specific hash breaks ties. Selection happens at the native route
choice, not as frame-by-frame steering jitter. Nightmare's direct shortcut or
closest approach restricts this variation first. A single authored shortcut will
necessarily be shared by multiple AI. Native preplanned-route flag and force
arguments are preserved; returned routes are logged as accepted or not accepted.
First 48 decisions log difficulty, candidate count, shortcut/approach and acceptance.

Synthetic C tests cover variation across identities at all six levels, stable
choice for one identity, occupancy avoidance, shortcut approach before the flagged
segment, hazardous exclusion, direct shortcut priority and Story bypass. Existing
drift/reserve/profile tests pass. Native authority input replication remains as
previously documented; this does not claim a new live multiplayer sync test.

Executable-only build completed 2026-11-01T07:58:18.712476, 2142776 bytes at out/sacredfire. GPU hash unchanged. User launch with BB_PREBUILT=1 and BB_PROBE="$PWD/out/sacredfire" regenerates updated Online assets through run.sh. No gameplay test performed.

## Authority-carried profiles and stable route choice (2026-11-01)

User requests explicit ownership, disconnect handling and varied Nightmare routes.
The previous process-local profile limitation is now addressed DURING AI input
replication, without adding an out-of-range native EDifficultyLevel or a separate
UDP/HTTP protocol. This is not a claim that lobby profile replication or native
host election has been rewritten.

Verified native wire layout:
- CKartAIInputs registration1908e82 gives +0c `_turnAnalog` and +10 `_bitfield`
  their own replication attributes. `_bitfield` factory1545320 registers
  igCharMetaField via1bbd832: this is a byte, not a six-bit enum.
- The six action aliases use bits0..5 only. dcb820 explicitly preserves maskc0
  when packing; dcb8e0 reads only masks1/2/4/8/10/20 and unchanged steering.
- dcb630 checks owning entity+3b bit0. The owned branch executes tactical/item
  logic and packs input; the follower branch only consumes replicated input.

Wrapper dcb630 stores extended profile0..3 in bits6..7 on the existing owner
BEFORE native packing, which preserves those bits. A follower learns those bits
from its replicated input and cannot run custom route/line decisions there.
Native authority branching, simulation actions and steering remain unchanged.
When native ownership moves, a former follower uses the last received profile
instead of its unrelated local menu preference. Native difficulty still carries
Easy/Medium/Hard and remains the gate on whether an extended profile applies.
No native difficulty3/4/5 is written into game tables or saves.

The learned room profile is separate from the user's offline choice and cleared
on actual room leave. Changing difficulty requires ownership of CDifficultyManager
using the same native ownership query as its getter; a guest change event is
ignored. An owning choice clears the followed profile. Arcade/Adventure do not
read/write the network profile bits. Actual native entity ownership, not a cached
"I was the host" flag, determines whether decisions are produced on every callback.

IMPORTANT limits: this carries the profile with racing AI input, not the pre-race
lobby selector. It follows native host/entity authority transfer; it does NOT elect
a replacement host, guarantee native migration succeeds, or turn a disconnected
session into an offline race. If native ends the room, the existing disconnect
flow remains. A replacement that never received the AI input has no profile state
to recover. No live host-disconnect test has been performed. Older builds ignore
the two bits but cannot retain the extended profile on takeover; use the same
updated build on both clients when testing this feature.

Route tie breaks no longer depend on process memory addresses. They use native
player network index (or AI ID), lap, candidate index and world endpoint bits.
Equivalent graph data at different addresses produces the same choice. Comparable
routes still spread by current occupancy; lap changes vary the tie break. A single
safe authored shortcut or single viable jump approach cannot produce multiple
geometric lanes; the patch does not invent off-track routes for visual variety.

Checks: exhaustive 4 profiles x64 action combinations, conflicting guest local
choice, follower decision suppression, simulated native ownership handoff, room
reset, offline isolation, stable identity across relocated racers, stable route
choices across relocated graph data, guest menu rejection. Existing route,
drift/reserve and team runtime tests also pass. USA/EU dcb630 prologue and resolver
addresses checked against the extracted binaries. These are code/static checks,
not a two-client gameplay or transport loss test.

## Developer Time Trial references decoded (2026-11-01)

Latest instruction: Nightmare should learn the developer Time Trial route on every
track, while retaining independent race decisions and avoiding train-like lines.
This section supersedes any implication that the authored-shortcut patch already
achieves that. The user's latest Arcade Normal test still missed Crash Cove's jump.
The 07:59 log shows route approach/shortcut choices accepted by native code; that
proves selection, NOT successful driving or jumping.

`scripts/tt_reference.py GAME --out OUT` now decodes the player's own ghost
files read-only. `loosedata/ghostdata.igz` contains 40 track entries with developer
slot 3 (after Tropy, Oxide, Velo), backed by 39 unique files. Retro Stadium uses
Turbo Track's reference. Output is under `out/tt-reference`: a JSON
manifest and movement/input CSVs for each track. No copyrighted ghost data is
added to source control.

Verified native recording:
- fcbec0 writes CGhostMoveKey world position, unsigned-short centiseconds
  (constant1cab1bc=100), signed-short angles (1cab1c0=10430.0595703125).
- fcc140 writes snapshot time at +0e and race-start bit14. Metadata18906bc maps
  turn signed-byte to bits0..7; accelerate/reverse/brake to bits8/9/10;
  first jump/second jump/start drift/race start to bits11/12/13/14.
- IGB version13, ghost version3. Serialized short fields occupy four-byte slots;
  upper bits may contain unrelated data and MUST be masked. Type indexes vary
  per ghost. Object order is not chronological: directory IDs interleave objects
  and memory entries; the tail memory lists reconstruct the actual key order.
  Equal-time snapshots are preserved in native list order, not arbitrarily sorted.

All 40 references decode to 2100 movement keys each. Crash Cove developer time is
79.74572754 seconds; recording starts race at 3.80 seconds and ends at 83.51.
Tests check all developer files, list order, counts, finish duration within .1s,
invalid section bounds, corrupt record lengths and corrupt directory references.

Status at 08:25 (superseded by the implementation below): feeding these references into live AI steering, matching
jump/drift events to current track progress, collision-aware lateral offsets,
item-crate routing/usage improvements. Exporting references does not enable ghost
playback or make Nightmare follow them. A useful implementation must match spatial
progress, not elapsed ghost time (hits, traffic and recovery change race timing),
and keep host authority. Do not teleport AI or copy one input tape to every racer.
Native e49860 already probes upcoming ground for jump intention; successful route
selection alone does not establish the correct takeoff speed or position.

CPU executable rebuilt: 2026-11-01T08:25:40.611114+07:00, 2148608 bytes at `out/sacredfire`. No GPU build and no gameplay launch. Runtime C checks pass; 17 online asset tests pass for both USA/EU dumps; full USA online preparation succeeds. Developer ghost decoder validates all 40 developer references.


## Spatial developer guidance and race item decisions (2026-11-01)

The remaining implementation is now connected, replacing the earlier export-only
status. Gameplay success remains unverified: the user explicitly owns gameplay
testing, so no game process was launched or controlled.

### Steering, jumps and varied lines

`runtime_tt_reference.c` loads a bounded `BBGHOST1` reference file generated from
all 40 local developer entries. `run.sh` prepares it for supported USA/EU 1.21,
including prebuilt/offline launches; a missing or invalid reference falls back to
native steering. Adventure is excluded by the runtime Arcade/Online gate.

Hook dd0210 runs native tactical driving first, then Nightmare replaces its two
world steering targets (AI+28/+34) using spatial progress along the developer
reference. Hits or a slow start do not advance playback by a wall clock. Local
projection, direction/height gates and spatial reacquisition handle deviations;
large discontinuities or an unusable reference retain native targets. Native
controller/physics/checkpoints/recovery remain in charge; no position, velocity,
reserve or item is fabricated.

Recorded drift sections increase drift-entry sensitivity within the existing
native predicate. Recorded jump edges set native AI jump intention; dcf740 still
requires its native grounded/state conditions. Lane preferences differ by racer
and lap and smoothly respond to nearby reference-following racers. The line
centers ahead of a recorded jump, and item-box attraction is suppressed there.
These are small offsets, not a road-width-aware overtaking planner. A single
narrow shortcut necessarily has shared geometry. Correct takeoff speed, landing,
and actual success on every track still need user gameplay evidence.

### Boxes and item targeting

All six Arcade/Online difficulty levels can aim at an active ground power-up box
within a bounded forward route corridor when their inventory is empty and no
roulette is running. The source is the native collectible manager 2acdc40, list
+10, crate active +2c / airborne +2d, data content +18 == 0. This ignores inactive,
airborne and Wumpa crates. Per-racer claims discourage chasing the same box;
claims do not reserve a box in the game or guarantee collection against traffic.
Itemless has no active power-up boxes to select; its driving tuning still applies.

The Race item cone scan dcdd30 now additionally checks the native enemy/eligibility
predicate dc9db0, which the native scan previously used only for Battle. A viable
unfinished enemy must be ahead of the requested forward/rear direction, in range
and in the cone. Native negative decisions stay negative. Battle preserves its
native policy. Native item categories, roulette, target-lock delay and use timers
remain authoritative; this is not a replacement for every item's homing physics.
Existing extended-profile tuning reduces target-lock delay. The roulette-interrupt
parameter affects native Battle, not a new Race early-roulette mechanism.

### Authority and validation

Only the owning AI entity applies guidance/box/target decisions online. Guests
retain native replicated inputs; there is no separate guest path simulation.
Existing profile replication and ownership handoff remain in place. Native host
migration/election is not replaced, and disconnect behavior is not gameplay-tested.

Validated: USA/EU hook prefixes and position/enemy helper addresses; 40-reference
export/loader; Crash Cove spatial queries; lane differences, recovery, height and
wrong-direction fallback, jump/drift intent, malformed-file rejection; active-box,
occupied-inventory, airborne/content, contested-box and jump-approach gates;
friendly/enemy/finished/rearward targeting and Adventure/Battle/guest exclusion.
Three decoder tests and C assertion suites pass with warnings treated as errors.
Shell syntax and git diff whitespace checks pass. Sanitizer linking was attempted
but the installed toolchain lacks `/usr/lib64/libasan.so.8.0.0`; no sanitizer pass
is claimed. CPU executable rebuilt at `out/sacredfire`; GPU was not built.
GPU SHA256 remains f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.

Runtime diagnostics: `Native AI: loaded 40 developer track references`, followed
by bounded `guidance ... developer=1 box=... drift=... jump=...` records. A loaded
reference or accepted jump intention does not by itself prove a completed shortcut.


## TT tiers and races longer than three laps (2026-11-01 follow-up)

User correction: Easy/Medium retain native route guidance. Hard uses N. Tropy,
Very Hard uses Oxide, Extreme uses Velo, Nightmare uses developer. All existing
per-level drift/boost/box tuning remains; references do not copy a shared input
tape. The owner selects guidance; peers continue native replicated inputs.

Renamed source/exporter/tests to `tt_reference` (`runtime_tt_reference.c/.h`,
`scripts/tt_reference.py`, `tests/test_tt_reference.c/.py`), environment to
`BB_TT_REFERENCE`, generated directory to `out/tt-reference`. Export now reads all
four native header slots per map: 160 track/tier records, 156 unique ghost files.
No game assets are modified. Historical developer-only status above is superseded.

The spatial cursor reacquires on each native lap change, including laps 4–7.
Lookahead wraps at the recording's end. The seam joins to the nearest forward
continuation on an earlier recorded lap instead of returning to the starting grid.
Discontinuous or otherwise unusable reference geometry falls back to native
steering; AI never receives a stop command because its reference ended.
Small per-racer/per-lap lateral offsets and nearby-racer avoidance still apply;
recorded jumps center the approach. This is bounded dynamic variation, not a
promise that racers can take different paths through one narrow shortcut.

Validation: all 160 local records decode; four Crash Cove tiers traverse spatial
queries at every lap 1–7; synthetic circular recording crosses its seam at lap 7;
Easy/Medium guidance gates reject references. Existing malformed-file, jump/drift,
height/direction, item, authority and race-options tests pass. Gameplay remains
user-owned; no claim that Nightmare successfully lands every shortcut.

CPU build completed: 2026-11-01T17:45:30.445692, 2188064 bytes at `out/sacredfire`. GPU unchanged; no gameplay launch.

## Nightmare technique follow-up: sustained drift, takeoff, reserves and U-Turn

User clarified the reported shortcut failures were on Nightmare, not Extreme.
Native ghost snapshot writer fcc140 stores turn, accelerator, reverse, brake,
first/second jump buttons and a start-drift pulse. Crash Cove developer recording
contains 26 start-drift pulses but 545 snapshots holding the first jump button;
reading only the pulse missed most of each slide. Counts describe snapshots,
not numbers of completed maneuvers. Reverse + brake + hard steering with
accelerator released appears in the TT data, including Crash Cove. These are
U-Turn input candidates; geometry alone does not prove a successful U-Turn.

Exporter now retains sustained drift and second-shoulder boost edges, plus the
U-Turn direction and held jump button. Direction comes from the current spatial
sample, not an OR of future left/right commands. Runtime keeps per-racer lane
variation and uses the selected tier's recording, not a shared timed replay.
Very Hard/Extreme/Nightmare temporarily enable native AI drift/boost permissions
while processing guidance. Drift sensitivity now follows reference intent across
those tiers. Empty active-fire lists now mean zero reserve, not infinity, so the
AI requests an earlier legal boost rather than waiting for a nearly full meter.
Native boost state, speed and maximum-count checks still decide whether it fires;
no reserve refill or speed injection is introduced.

Native slide can keep Jump held across a reference takeoff. The patch releases it
for one grounded frame then presses again, once per reference jump window. It
cancels a queued press on becoming airborne. U-Turn reverse/brake/steering is
applied only while airborne; the patch does not apply this braking combination
on the ground. Both changes pass through native packed-input decoder dcb8e0.
The authority modifies the replicated action byte; peers retain native decoding.
Item and difficulty bits are preserved. Adventure and ordinary player inputs are
excluded. A reference maneuver may still be unreachable at the AI's actual speed
or reserve state; gameplay validation is required, especially shortcut landings.

Validation: four Python tests include decoding all 160 local track/tier records,
sustained drift, boost edges and U-Turn flags. C tests cover airborne/grounded
U-Turn, left/right direction, profile/item preservation, jump release/press and
landing cancellation, empty reserves, boost timing, authority/Adventure gates,
and reference guidance through laps 4–7. All pass. The native decoder and GUI
broadcast entry bytes were inspected in the local extracted ELF. No game launch.

CPU build completed: 2026-11-01T17:59:57.971867, 2197984 bytes at `out/sacredfire`. GPU SHA256 unchanged: `f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e`. No gameplay launch.

## User confirms route/hops but no drift: native shoulder hold (2026-11-01)

User gameplay observation supersedes the earlier optimistic drift descriptions:
reference routing and hops are visible, but drift was not. The previous start
predicate adapter only scaled an angle. Native dcfb10 still rejects the nearly
straight path by its curvature/sign gates, and dcfc10 can release the shoulder
when its own continuation predicate fails. Raising sensitivity is insufficient.

TT records now carry left/right drift-start steering separately from U-Turn.
Sustained shoulder holds include neutral steering snapshots, rather than losing
the slide whenever steering crosses zero. Very Hard/Extreme/Nightmare authority
now holds the packed native jump/drift action over that reference interval.
For entry/landing states, recorded direction supplies a bounded 0.65 steering
command; established slides retain native path/countersteering. Neutral reference
steering retains an already active slide. Minimum normalized speed uses native
f4afc0 (EU f4afb0) and controller +4c, including its recovery threshold. Native
state range is checked; recovery/finished/Adventure/non-owner exclusions remain.
During a slide, f4c840 must approve the 0.8 boost threshold before the second
shoulder action is set. No fire/reserve injection is performed. This changes
actual held input rather than only geometry. U-Turn and takeoff release/press
logic retain precedence, and item/profile bits are preserved.

Tests cover entry direction, shoulder hold, neutral steering continuation, native
boost approval, minimum speed and unchanged unreferenced input. Reference tests
cover serialized directions and neutral holds across the 160-tier export. Native
speed helper bytes checked against the USA ELF and EU text dump. Gameplay remains
user-owned: drift appearance and shortcut landing are not verified by unit tests.

Validation completed: C race-options, online-mode (both regional fixtures), TT guidance tests, and all four Python reference tests pass. Export regenerated 160 references. CPU rebuild: 2026-11-01T18:12:25.630018, 2199680 bytes. GPU SHA256 remains f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e. No gameplay launch.

## Power-slide cycle and reserve-aware boost follow-up

User emphasizes actual power-slide technique and preserving reserves, not merely
holding drift. Removed the reference-input path's fixed 0.8 boost decision: it
now calls the same reserve-aware decision used by native AI. With safe reserves,
Very Hard/Extreme/Nightmare target .92/.95/.97 of the native timer. A recorded
second-shoulder edge alone no longer lowers that threshold. An imminent takeoff
or fastest-fire reserve below one second still permits the native .8 timing;
this trades some charge for retaining fire. Native state/speed/gauge/count checks
remain mandatory. Existing second-shoulder input is cleared while waiting, so a
previous native decision cannot bypass the chosen threshold.

Native f4c520 checks exhausted consecutive boosts; f4c780 checks slide oversteer
with the original 1/30 margin (constant 1ca9dec). Reference shoulder holding now
releases drift/boost when either gate trips. After native state returns to normal,
the next reference slide can begin. This prevents holding a completed boost chain
forever and retains the native oversteer safety release. No transforms, unlimited
reserve flags or boost durations are written. Human/Adventure paths remain native.

C tests cover safe reserves, expiring fastest fire, reference edge versus takeoff,
native refusal, exhausted-chain release, oversteer release and next-cycle input.
They pass. Near-full timer targeting is not proof of a PERFECT rating at every
physics tick, nor a guarantee of retaining fire through collisions/shortcuts.
User gameplay verification is still required; no game was launched.

Latest CPU build (includes power-slide cycle follow-up): 2026-11-01T18:14:31.116046, 2200264 bytes. C tests pass; GPU checksum unchanged. No gameplay launch.

## 18:14:49 host crash and room difficulty lifecycle (2026-11-01)

Host log `20261101_181449.log` ends with SIGSEGV at guest offset 3dcfeb.
Stack: 3dcfeb <- e318d9 <- f4afce <- host 44132f, symbolized to
apply_tt_technique at the normalized-speed call. e318b0 loads kart+18 and
passes it to 3dcfb0, which dereferences that entity without a null check.
The recorded null fault establishes a null kart entity in this call path.
This is an adapter regression; unrelated GPU stall/write-check messages are
not evidence of the crash cause.

The technique adapter now requires the resolved kart's entity to be non-null
and identical to the AI entity before any technique helper/input mutation.
Speed is queried only for a reference drift, not every route sample. Tests
reproduce null and mismatched entity bindings and assert no helper call/input
mutation, then verify the restored binding permits normal drift processing.

User also requests all guests follow room difficulty through takeover but regain
their own difficulty at lobby return. Native c0a3b0 reads manager+0c for an owner
and +10 for a non-owner. Preserving only the extended profile was insufficient:
manager takeover could choose the guest's personal base difficulty. The adapter
now retains the received native base plus extended profile for active racing.
It wraps that getter with the same native ownership/field selection otherwise;
no personal settings or saves are overwritten. Difficulty getter/ownership entry
bytes match both regional text dumps and installation checks the getter prefix.

At the existing map-load boundary, online-lobby entry clears the followed base
and profile and selects the personal +0c field. A new race releases the lobby
view and adopts the next authority's existing replicated base/profile. Local
host selection and leaving the room also clear the followed override. Guests
still cannot change the host's difficulty. Existing native low-six action bits
and two extended profile bits are unchanged; no new transport is introduced.

C coverage tests every personal/host pair across all six levels (36 combinations):
follower behavior, simultaneous entity/manager takeover, unchanged personal base,
lobby restoration, and following the next race. Crash guards and existing online
fixtures pass. This does NOT implement or prove host election: a surviving guest
without native AI ownership remains a follower and can stop receiving AI updates.
Only a host log is available here; user reports guest AI froze after the crash,
but there is no guest migration trace establishing why authority did not transfer.
Do not claim live migration or post-crash recovery is verified.

Final validation also covers late race-profile state while in lobby: it cannot override personal difficulty and is cleared before the next race. CPU build: 2026-11-01T18:23:57.250685, 2202568 bytes. C checks pass; GPU unchanged (f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e). No gameplay launch.

## Nightmare shortcut entry: exact jump markers and bounded approach (2026-11-01)

User still reports shortcut failure. Latest log 20261101_183312 confirms Arcade
Crash Cove, Nightmare selection and 160 loaded TT records. It records guidance
intent, not actual launch speed/landing, so no successful shortcut is inferred.

Concrete timing defect: export marked a first-shoulder edge over +/-0.10 seconds,
and runtime ORed future jump flags through +0.12 seconds. This could trigger a
jump up to ~0.22 seconds before the recorded takeoff, before additionally applying
the release/press cycle. A faster reference and a slower AI cannot share that
broad time window at a narrow shortcut lip.

Each input edge now maps to exactly one nearest spatial move sample. Guide does
not OR future jump flags. It recognizes the current marker or a marker crossed
between adjacent updates (bounded to four samples, excluding large reacquisitions).
A separate approach flag releases the held shoulder 75–200 units before the edge,
scaled by actual horizontal velocity, without jumping. At the marker, press the
shoulder immediately if prepared; otherwise preserve the safe release/press
fallback. An already-airborne marker is consumed without queuing a landing hop.

Near/far lookahead is bounded by current velocity; jump approaches cap them at
180/420 units rather than allowing a 1200-unit target to cut across the entrance.
Approach lane offset becomes zero immediately instead of decaying over many
frames. Per-racer/lap variation still applies outside those approaches. Drift
entry now retains the signed recorded steering magnitude (packed in bits16..23),
not just its direction with fixed .65 input. Active-slide countersteering and
reserve-aware legal boost policy remain native/adapted as documented above.

Nightmare still resolves the developer tier; no track teleport, velocity injection,
infinite boost or shared timed ghost playback is added. Bounded diagnostics record
actual/reference speed and path error at jump markers, accepted input/grounded
state, and missing technique bindings, to distinguish route intent from applied
actions on the user's next run.

Tests cover single-marker export, no premature jump, preparatory release, press
at the marker, skipped-sample crossing, target bounds, recorded steering magnitude,
all 160 reference exports and Crash Cove tiers across laps 1–7. These checks do
not establish that every shortcut is physically completed; gameplay remains
user-owned and no game was launched.

CPU build completed 2026-11-01T18:40:49.230958, 2210480 bytes. Five Python tests, C race-options and TT guidance checks pass. GPU hash unchanged. No gameplay launch.

## Confirmed wrong AI component binding: drift/boost never applied (2026-11-01)

User still sees hopping without drift/perfect boost. Latest 184111 log records
`technique skipped ... entity_match=0` twice for each of seven AI, and no
`takeoff input` records. Previous guidance logs described intent only. The
adapter's technique path was not reaching its packed-input decoder.

Root cause verified in native initializer dcae80: lookup metadata 2ac39d0 is
stored into AI+48 (driving), 2ac38a8 into +50 (tactical), and 2ac3a38 into +58
(item use). Earlier patches mistakenly treated AI+58 as driving. Thus permission
writes were targeting item-use fields and its +58 value was misinterpreted as a
kart handle. This also explains the 181449 normalized-speed crash; the preceding
detached-entity explanation was incomplete. The guard prevented invalid calls
but could not repair the wrong binding, so the following builds skipped technique.

Corrected all three sites: permission save/restore around the AI callback,
reference-guidance permission enable, and technique controller/kart lookup now
use AI+48. Controller+58 remains the actual kart handle. Entity identity guards
remain. Native initialization lookup/store bytes are verified in both USA/EU
text dumps and checked before hook installation. No change to GPU or game assets.

Previous unit fixtures duplicated the wrong +58 assumption and therefore missed
this error. Regression fixture now models distinct driving at +48 and item use
at +58, with sentinel item-use bytes. It verifies the complete wrapper reaches
input application with both shoulder actions set, restores driving permissions,
and leaves the item component untouched. Existing drift direction, near-full
boost timing, reserve urgency, three-boost-cycle release, crash guards and
room/authority lifecycle checks pass with the corrected layout.

Bounded `power slide` diagnostics now report controller slide state, normalized
speed, native boost count and packed actions when both shoulder actions are
actually applied. An applied input remains distinct from a measured PERFECT
rating or a successful shortcut landing. Those require user gameplay validation;
no game was launched by the agent.

CPU rebuild: 2026-11-01T18:47:38.674270, 2211272 bytes. Corrected-layout C regressions pass, including native meter refusal clearing boost while preserving drift/item/profile bits. GPU hash unchanged; no gameplay launch.


## 2026-11-01 — stop conflicting ghost/native drift steering

User reports violent left/right drift changes with the 18:47 build. Latest log
20261101_184807 now contains actual power-slide input applications and native
boost counts 0/1: the corrected component binding works, but does not establish
successful technique. Some reference takeoffs still report path errors above
1000 units, so shortcut completion is explicitly not established.

Found a control conflict: apply_tt_technique overwrote native steering with raw
recorded steering during states 0/1/2, then returned to native countersteer in
states 3/4. A spatially nearby ghost sample does not share this kart's heading,
lateral error or drift phase. The adapter also forced shoulder hold after native
dcf1c0 refused to continue a slide, bypassing its path/heading decision.

Removed normal drift steering overrides. Native dcf1c0 now owns entry, landing
and countersteer without switching to recorded full-lock inputs. Technique
boost timing only applies when native packed input already holds the shoulder
AND driving+74=1 (dcf1c0 slide); +74=2 is a dcf740 hop and is not a slide.
Native refusal remains a refusal. Reference route guidance, frequency tuning,
permissions, takeoff preparation, airborne U-turn handling and boost/reserve
thresholds remain. This is not exact ghost-input playback or proof of equivalent
developer lap pace. Airborne U-turn still uses recorded input and is a separate
remaining source of possible pose mismatch.

Regression exercises opposite recorded steering across all five native states,
native slide cancellation, distinguishing hop from slide, legal boost timing,
component identity and owner/follower behavior. C race-options suite passes.
No gameplay launched; actual drift stability and shortcut success require user
validation. CPU-only rebuild follows; GPU excluded.

CPU build completed 2026-11-01 18:53:58 +0700: out/sacredfire, 2211064 bytes. GPU SHA256 unchanged (f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e). git diff --check clean.


## 2026-11-01 — actual recorded slide phases and first-boost reserve policy

User reports Nightmare still hops without useful powerslides/reserves. Latest
185626 log contains repeated power-slide command diagnostics with boosts=0;
these are input requests, not evidence of accepted PERFECT boosts. The earlier
control-binding fix and steering handoff fix were insufficient.

Three concrete defects corrected:

- Export treated every first-shoulder hold as drift, including ordinary hops.
  It now identifies contiguous shoulder holds containing a start_drift pulse.
  The whole matching hold (including its entry hop) is a slide reference; a
  standalone hop is not. A transition between spatial samples is retained.
  Entry direction is latched from start_drift rather than following subsequent
  opposite countersteer. Runtime still leaves actual steering to native control.
- dcfb10 checks both controller+44 curvature and its heading-angle argument
  against native 0.15, plus direction and normalized-speed guards. Previous
  frequency tuning scaled only heading and could never open the curvature gate
  for shallow recorded slides. Both angular inputs now receive sensitivity;
  a recorded slide with agreeing nonzero angles above 0.025 can cross the 0.15
  entry threshold. The original predicate retains kart/speed/handle checks.
  The temporary curvature value is restored immediately; native feedback and
  countersteering are not overwritten for the driving update. This is an entry
  assist, not literal recorded steering playback or an unconditional drift.
- No active fire returns reserve=0. The prior urgency condition treated that
  as expiring fire and used 0.8 timing for every initial boost. Now all profiles
  use their intended full-meter target first (Nightmare 0.97); emergency 0.8
  applies only to positive existing fire near expiry or a takeoff deadline.
  No-fire takeoffs do not downgrade the first boost. Actual game meter/state
  gates remain authoritative; no synthetic reserve injection.

Added bounded accepted-slide-boost diagnostics when native kart+ec actually
increases, including remaining fire duration. This distinguishes a pressed
button from an accepted boost; it still does not claim the native PERFECT rating
or successful shortcut landing. Existing ownership replication is unchanged.

Regression covers drift-start pulses between samples, direction latching across
countersteer, ordinary hops, no-fire first boost, genuine expiring fire, both
angular gates, opposing heading, zero heading, temporary-field restoration,
Adventure exclusion and native stuck-speed refusal. Six Python reference tests
pass against local assets. No game launch or independent gameplay testing.
Nightmare matching developer pace/shortcut reliability remains unproven, and
actual native continuation can still reject a slide when the AI pose departs
from its reference. Do not report the overall difficulty work as complete.

Validation: C race-options and TT guidance tests pass; exported all 160 references. Crash Cove developer record has 2098 samples, 1644 slide-reference samples and 39 jump markers, including 14 plain-hop markers now excluded from drift intent. CPU-only build completed 2026-11-01 19:04:38 +0700, out/sacredfire 2216872 bytes. GPU hash unchanged (f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e). git diff --check passes.


## 2026-11-01 — 19:52 user log: boost chains still fail

Inspected 20261101_195203.log after user reports no maintained speed/fire/reserves.
Only four accepted-slide-boost records appear: AI 7, 2, 6, 1; all count=1,
remaining fire 1.701, 0.000, 1.700, 1.697 seconds respectively. No accepted
second/third boost was recorded. Diagnostics are bounded, so do not treat sampled
output as a complete per-frame history. Across 69 moving takeoff samples, median
AI/reference speed ratio is 0.793526, median spatial path error 55 units. The
actual data does not support prior claims of adequate reserve management or
developer-equivalent technique. It does show proximity to the route is not enough.

Identified two uncovered control paths: native dcf1c0 -> dcfc10 decides whether
to KEEP a slide, whereas previous patches tuned only dcfb10 entry. Native
continuation uses direction-dependent signed heading bounds -0.125/+0.125 and
separately rejects complete boost chains/oversteer. Also every recorded shoulder
edge still set AI+7d and released the shoulder before takeoff, including the
faster ghost's next drift-entry hop while this AI was still charging its slide.

Changes:
- Runtime guidance labels close/aligned poses (3D projection error under 200,
  horizontal speed above 100, tangent dot above 0.85) and distinguishes current
  or upcoming drift-entry hops from ordinary hop markers. This also preserves
  classification when crossing a marker between spatial updates. No file format
  change; the extra flags exist only in runtime intent.
- Before native driving consumes AI+7d, suppress only a reference drift-hop
  restart when close/aligned, currently sliding, and native complete/oversteer
  predicates both allow continuation. Keep actual mandatory plain-hop markers,
  completed chains, recovery and off-line poses unchanged. Bounded log reports
  retained unfinished slides.
- Added an AI-only continuation hook at dcfc10. While close/aligned inside a
  recorded powerslide, a signed 0.125 margin expands the corresponding native
  heading cutoff to -0.25/+0.25. Native function still decides using its existing
  complete-chain/oversteer checks; its countersteering calculation uses the real
  heading, not the adjusted predicate argument. No raw ghost steering injection,
  no speed/reserve refill, no suppression of physical damage/collisions.

This margin is an explicit tuning choice, not a measured optimal value or proof
of developer pace. It can still be insufficient and needs user gameplay evidence.
Adventure, unguided modes and guest follower decisions keep the native predicate.
The same owner creates the packed actions; no guest-local AI authority added.

C regressions cover signed cutoff limits, native safety refusal, unaligned or
non-drift intent, Adventure/follower exclusions, marker classification across
updates, unfinished versus complete/oversteered slides, and mandatory plain hops.
USA/EU dcfc10 prefixes verified against both text dumps; the 14-byte prologue
trampoline resumes at dcfc1e on an instruction boundary. Runtime installation
checks this signature. Tests pass, including seven-lap reference traversal.
No game launched or controlled. No GPU build.


Shortcut safety follow-up before final build: a shoulder hold containing
start_drift is NOT proof its entry hop is dispensable. The first Crash Cove
shortcut marker at ghost time 13.28 has about 596 units of rise over the next
0.5 seconds. Runtime restart classification now excludes any marker whose next
0.5 seconds rises OR drops more than 100 units. This conservative heuristic
preserves the tested Crash Cove climb; it is not authored collision/terrain
metadata and cannot prove every track's hop classification. Regression covers
positive/negative height changes and the actual first-lap Crash Cove asset.

Final CPU-only rebuild including height classification: 2026-11-01 20:09:37 +0700, out/sacredfire 2219912 bytes. Race-options and TT-reference C regressions pass. GPU SHA256 unchanged: f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e. git diff --check clean. User gameplay outcome remains pending.


## 2026-11-01 — 20:11 log: reject invalid/repeated ghost takeoff commands

User still reports unprofessional-looking random hopping. Latest available log
201130 includes ghost takeoff application at speed=0 (AI7 error82.7, AI4 error609.7)
and AI6 moving takeoff error1015.4. Repeated takeoff-input logs while airborne
are not proof of repeated physical jumps, but the code did expose the same
spatial marker every update and could rearm after projection jitter. Existing
close/aligned gating applied only to slide continuation, not hop/U-turn issuance.

Fix marker consumption in the reference cursor: each of up to2100 markers has
one consumed bit per owner/track/lap. Stationary/off-path markers are consumed
without executing their action, preventing late replay on reacquisition. Existing
close/aligned criteria now gate reference technique flags and preparation as
well; spatial route recovery remains available. Preparation skips consumed
markers. New owner/track/lap resets the bitmap, preserving 5/7-lap traversal.

Fix release/press logic: adapter previously inspected the current packed R1 bit
AFTER native driving had run. A fresh native press looked like an old held button,
so the adapter could cancel the jump just requested. Capture previous packed
actions before native update and use those for edge decisions. If a prior hold
really needs release, finish the pending press on the next aligned/grounded tick
even though the spatial edge is now one-shot. Cancel pending press if airborne
or no longer aligned. Do not queue a landing hop. The owner alone produces input;
follower decoding/profile replication remains unchanged.

Regression covers stationary and distant-route rejection, repeated same sample,
backwards/forwards jitter, new-lap rearming, off-path consume/reacquire, fresh
native press preservation, one-shot release/press completion and cancellation.
C race-options and reference suites pass (including actual Crash Cove mandatory
climb and seven-lap fixtures). No gameplay launched. This fixes erroneous action
triggers, not demonstrated developer-equivalent driving or sustained reserves.

CPU-only build completed 2026-11-01 20:19:08 +0700: out/sacredfire 2220960 bytes. GPU SHA256 unchanged (f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e). git diff --check clean. No gameplay launch.


## 2026-11-01 — 202203 log: separate native control from TT action conflicts

Latest user log: logs/20261101_202203.log. Its accepted-boost trace contains
39 records: 35 first boosts, four second boosts, no third boosts. Trace caps mean
this is not a full-race census. 111 takeoff input records contain 105 actions09
and six actions01. They do not establish 111 physical jumps. Previous logging
cannot identify native versus injected braking or why each slide ended.

Native dce820 calls dcf1c0 for slide control, falling back to dcf740 for generic
hops, and stores slide status in AI+7c. The patch previously retained AI+7d's
native-route hop while steering toward a different TT route. For aligned guided
poses, TT now exclusively supplies terrain-hop timing; native dcf1c0 exclusively
supplies flat slide-entry hops. Flat-slide markers clear both jump and preparation
requests, preventing unnecessary shoulder release during an ongoing chain.
Unaligned/recovery native behavior remains available. Flat/terrain classification
is still the documented height heuristic, not verified authored terrain data.

Box detours previously remained eligible during reference drift (intent1).
They are now excluded during reference slide/hop approach or native slide AI+7c,
so collecting a box cannot replace the immediate slide target. Native item-fire,
steering, gas, reverse and brake bits remain unchanged by the technique adapter.
Existing native target filters are retained; perfect box collection and optimal
item use are NOT demonstrated by this change.

Removed the remaining raw ghost U-turn steering/brake/reverse injection: inputs
for the ghost pose are unsafe for a divergent AI pose. Proper adaptive U-turn
technique remains unfinished; this change does not claim to implement it.
Native dcf880's normal brake decision is gated to modes9..13; dcf8f0 can instead
withhold acceleration through AI+84's timer or delegate through AI+79. Therefore
visual slowing must not be equated with braking without observing input bits.
No forced throttle, speed injection, or reserve refill added.

New `Native AI: control` records compare native/final low-six action bits and
steering, plus native/final hop request, kart state, grounding, recovery,
throttle_alt (AI+79), coast_delay (AI+84), slide mode, box claim, normalized speed,
boost count, reserve duration and ready97 (native threshold predicate, NOT a
measured PERFECT rating). Bits:1 gas,2 reverse,4 brake,8 first shoulder,16 boost
shoulder,32 item. Records emit on state/boost/input transitions or every60 ticks,
limited to600 per AI owner with an explicit limit record. Owner-only execution
is unchanged; guests do not generate competing technique decisions.

Regression checks preserve all64 action combinations' gas/reverse/brake/fire and
profile bits, preserve steering for ghost U-turn flags, distinguish flat slide
from terrain-hop authority, and block box detours during native/reference drift.
No game launched. Gameplay quality, shortcut success and sustained reserves still
require user evidence; these changes remove identified conflicts, not prove
Nightmare matches the developer reference.

Validation: race-options and TT-reference C suites passed, including real local
reference assets. CPU-only build completed 2026-11-01 20:36:47 +0700,
out/sacredfire 2222208 bytes. GPU SHA256 unchanged:
f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.
git diff --check passed. No gameplay launched or controlled.


## 2026-11-01 — 203822 trace: native slide rejection and premature hop preparation

Latest available log 20261101_203822 contains3614 control samples, no trace-limit
markers. Across per-AI consecutive recorded samples:520 transitions from slide
state3/4 to non-slide (all state0). The preceding sample already has native R1
released in483 cases;27 have native R1 held but adapter R1 released;10 still have
R1 held in both.428 exits have zero preceding reserve duration. This is sampled
trace evidence, not exact per-frame causality. Accepted-boost capped records:
37 first boosts,6 second boosts,no thirds. No native or final brake-bit samples.
Thus these observed slowdowns are not explained by injected braking in this run.

Rechecked native dce820/dcf1c0, dcfb10, dcfc10 and f4c840. Native dcf1c0 reenters
state0 with an entry hop when dcfb10 accepts; dcfc10 rejects continued slides on
heading, completed boost chains or oversteer. Existing tuning made opportunistic
native slide starts more frequent even when the TT pose was unaligned or no
reference slide existed. This can produce repeated failed entries. For guided
VeryHard/Extreme/Nightmare only, entry now requires an aligned reference slide
window. The actual native direction/speed gate still decides. Unguided behavior,
Easy/Medium/Hard, Adventure and network followers retain their previous gates.
This does not prove every remaining native cancellation has been diagnosed.

Removed the adapter's repeated R1 release throughout a future hop's preparation
window (intent512). Approach leaves an active slide intact. Only the actual
one-shot terrain marker requests a release/press pair, if the previous frame
held R1; a fresh native press is preserved. Grounding/alignment cancellation of
the pending press remains. This moves takeoff at most one input update later
than an already-prepared press; shortcut timing requires user validation.

When the heading continuation gate rejects an aligned reference slide but the
native boost predicate already accepts .8 and the chain is neither complete nor
oversteered, permit the native slide path to spend that legal boost before release.
The per-update decision tells the native and adapter boost policy to use .8 for
this imminent exit. After the real boost resets its meter the predicate no longer
accepts, so the route gate can release normally. This salvages an available boost;
it does not hold a slide indefinitely, force a PERFECT rating, refill reserves,
write speed, or override oversteer/completed-chain checks. Owner remains sole
producer of replicated AI input. No forced ghost steering or braking introduced.

Extended control trace with entry decision (-2 reference-window rejection,-1 not
called,0/1 native result), entry heading/curvature, continuation native result and
angle/direction, complete/oversteer predicates on rejection, and bank flag. These
fields distinguish native heading rejection from physical/safety termination and
show whether boost-before-release actually executed. Old logs cannot make that
distinction, so sustained fire/developer-like pace remains unverified.

C race-options and TT-reference regressions pass: early preparation preserves R1,
actual terrain marker creates one release/press, off-path and non-slide reference
windows cannot start a guided slide, valid aligned native starts remain possible,
boost-before-release requires a legal meter and rejects completed/oversteered or
unaligned slides. Enlarged the technique fixture to include AI+84 used by traces.
No gameplay launched. No GPU build.

Validation completed: Clang ASan/UBSan race-options suite passed (GCC sanitizer
link unavailable locally). CPU-only build 2026-11-01 20:47:26 +0700,
out/sacredfire 2224032 bytes. GPU checksum unchanged:
f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.
git diff --check passed. User gameplay validation remains pending.


## 2026-11-01 — 205010: reference slide hold replaces stock-route heading cutoff

User confirms all tested AIs now make the shortcut; remaining issue is short,
interrupted drift and poor reserves. Latest log205010 has4009 control samples.
536 recorded continuation rejections all report complete=0,oversteer=0,bank=0.
Examples: AI7 tick47 direction1 angle+.250, AI4 tick54 direction0 angle-.280,
with aligned slide intent810501. Both have zero boosts/reserves. The existing
expanded native heading cutoff still ends the reference slide before its meter
is ready. Accepted-boost capped records25 first boosts,3 second,no thirds.
The boost-before-exit salvage policy never fires in this trace (bank=0 throughout).

For aligned reference slides on guided VeryHard/Extreme/Nightmare, use recorded
hold duration instead of the stock-route heading cutoff. Call the native predicate
with the actual heading first (preserved in trace); if rejected, direction0/1 and
finite angle, call the same predicate with neutral heading0. This neutralizes only
its route-heading gate. Native dcfc10 still checks actual complete/oversteer
conditions; native dcf1c0 countersteers using the real pose, not the neutral value.
Once the reference slide ends or spatial alignment is lost, stock heading policy
applies. Removed the unsuccessful bank-on-heading-exit workaround and its .8
threshold override. Existing native legal boost timing and positive-low-reserve
emergency policy stay unchanged; no forced perfect result or reserve injection.

Trace now reports reference_hold when the reference substitutes duration after
native heading rejection. Tests cover wider heading acceptance only within aligned
recorded slides; native complete/oversteer rejection; unknown direction, lost
alignment, follower and Adventure exclusions. This is a source correction grounded
in the new trace, not proof of sustained fire or perfect driving. No gameplay run.

CPU-only build completed 2026-11-01 21:04:47 +0700, out/sacredfire2231080 bytes.
Race-options Clang ASan/UBSan passed; runtime and asset regressions passed.
GPU SHA256 unchanged: f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.
git diff --check passed. No gameplay launched; user validation pending.


## 2026-11-01 — Normal needs pickups as well as a fast line

User reports improved shortcuts/drifting but poor reserves and insufficient item/
Wumpa box collection in Arcade Normal. Latest log210525 contains2539 control
samples:2538 box=0,one box=1. Capped accepted-boost records67 first,6 second,one
third; counter snapshots contain boost3 in5 samples. Reference-hold override is
active in263 samples. These records show some progress, not sustained reserves
or a complete race census. Numerous native heading refusals occur when the ghost
hold bit has dropped although the actual AI slide is unfinished.

Confirmed two pickup exclusions in existing code: content had to be0(item), and
intent1(reference drift) blocked planning even before a real slide started.
Additionally the entire .8-second lookahead before EVERY ghost hop—including
ordinary flat drift entry—set the shortcut-approach veto. Normal consequently
spent most of its reference line unable to select a pickup.

Native evidence from prepared image reflection: CRacer::_currentWumpaFruitCount
is+9c; _perfectDriftBoostInARow is+10c. CrateContent names at25e51b0 are0PowerUp,
1WumpaCrate,2WumpaFruit,3SwitchCrate. Do not treat switch/hazard or loose fruit as
an item box. No guessed memory offsets added.

Pickup policy now considers live grounded item boxes when the item inventory is
empty and roulette inactive, and Wumpa boxes when actual fruit count is0..9.
Wumpa remains eligible with an item held or roulette active. At10 fruit stop
seeking Wumpa. Prefer nearby forward boxes; low fruit slightly increases Wumpa
utility. Retain a viable previous target (score factor.7) to avoid weaving between
near-equal options; existing per-AI claim penalty still discourages convoys.
These are explicit utility heuristics, not measured optimal race strategy.
Existing distance/lateral/height limits and active-state checks stay in place.

Reference drift intent alone no longer blocks planning. Once a pickup approach is
chosen, clear ghost slide/alignment intent for that update so ghost entry cannot
fight the box target. A real active native slide still blocks detours. Actual
terrain/shortcut approaches and takeoff still block detours. The future-hop veto
now distinguishes terrain hops from the existing flat-slide-hop classifier;
flat drift entries no longer veto the whole upcoming interval. Native Itemless
removed/inactive/out-of-range boxes remain excluded. Loose Wumpa pickup planning
is not part of this change. Item firing/targeting/native reward physics unchanged.

For ongoing guided slides, finish the actual native boost chain across brief
recorded button-release gaps while spatially aligned. New slide entry still
requires an aligned reference slide window. Continue override stops on actual
terrain takeoff/U-turn intent, lost alignment, completed chain or native physical
oversteer. A future terrain approach alone does not force early release. No fire
refill, speed injection, forced boost approval or assumed PERFECT rating.

Control trace now includes pickup kind, current Wumpa count and the native perfect
boost chain counter. Bounded pickup-target transition logs show kind and demand.
Tests cover Wumpa demand/full/unknown count, held item and roulette, excluded switch
crates, sticky targets, active-slide and shortcut protection, flat-vs-terrain
lookahead, reference-release continuation and native safety/takeoff gates.
CPU/asset guidance remains owner-only online. No gameplay launched, no GPU build.
Sustained reserves and actual pickup success still require user validation.

Validation: race-options and TT-reference C suites passed, including actual local
reference assets; Clang ASan/UBSan race-options suite passed. CPU build completed
2026-11-01 21:17:25 +0700, out/sacredfire2232920 bytes. GPU SHA256 unchanged:
f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.
git diff --check passed. No gameplay launched.

## 2026-11-01: steering regression and missing recovery evidence

User reports collisions, interrupted perfect chains and unstable reserves in
Normal Arcade. Latest log 212318 contains startup only; latest complete gameplay
is 211829. Its 2,438 guided control samples contain 13 Wumpa-box and 7 item-box
target transitions. Wumpa maxima for IDs 1–7 are 10/4/7/5/6/9/10. This proves
Wumpa count increases, not which particular crate supplied it. 31 consecutive
recorded samples fall from speed >.7 to <.35; samples can be 60 ticks apart, so
this alone does not prove collisions. All recorded recovery values were zero:
the old trace omitted native-only, pickup-only and recovery updates.

Found an unintended steering change in the previous pickup patch: removing the
terrain veto from flat drift-entry hops also removed their lane centering and
short lookahead. Separate center_hop from the pickup-veto flag, restoring steering
geometry while permitting flat-ground pickups.

Reference projection previously selected targets up to 1600 units away even
after technique alignment was lost. Outside the existing 200-unit technique
corridor, retain native targets instead of overriding recovery with a direct
pull toward the reference. Consume crossed takeoff markers before yielding so
reacquisition cannot replay missed jumps. Log bounded transitions with path error,
speed and sample. This is conservative fallback, not an obstacle/navmesh solver;
actual recovery and shortcut success remain gameplay validation items.

Control diagnostics now cover native-only/recovery/box-only updates, preserve
owner-only decisions, and include native crateCollectedCount (+104), guided flag,
perfect-chain count and actual reserves. No reserve refill, forced PERFECT,
speed injection or invented item behavior added. Perfect-chain/reserve stability
is still unconfirmed, rather than claimed fixed from static tests.

Validation: race-options and TT-reference C suites passed, including local
160-track reference data; race-options Clang ASan/UBSan passed. Regression tests
assert unchanged native targets outside the corridor, no missed-hop replay on
reacquisition, and centered short lookahead for flat hops without blocking boxes.
No gameplay launched. CPU-only build; GPU library unchanged.


## 2026-11-01: itemless pickup policy and TT recovery hysteresis

User requests box pursuit only in Normal private/Arcade, with Itemless keeping
racing/boost/shortcut priorities. Gate aim_at_box using runtime_speed_race_active,
the same active-private Itemless predicate used for assets/contact behavior.
Clear any old claim before returning; leave targets and TT technique intent
untouched. Normal remains eligible for item and Wumpa crates. Tests exercise
both crate kinds, clearing retained claims, unchanged targets, and Normal resume.

Inspected213325, the latest complete gameplay log at investigation time;213825
had no control samples yet.213325 contains411 TT recovery transition reports,
362 at path error180..230. This exposes repeated handoffs at the single200-unit
threshold. IDs1/4 record122/241 native recovery samples respectively; all sampled
perfect_chain values are0. Samples are transition/periodic and capped, so these
are diagnostics, not collision counts or proof of full-race performance.

Retain200-unit recovery entry but require error below120 to rejoin TT. The
technique-aligned flag respects the same state. Consume crossed hop markers
while recovering, as before, preventing late jump replay. Regression exercises
190/205 boundary oscillation, rejoin at119, continued guidance at190 after
rejoin, and unchanged native targets during recovery. This addresses unstable
controller handoff; does not establish that all collisions or reserve/chain
problems are solved. In-game validation remains required.

Validation: race-options and TT-reference suites (including160-track local
reference asset) passed; Clang ASan/UBSan race-options passed with existing
string-literal pointer comparison warning suppressed. CPU-only build completed
2026-11-01T21:41:24.461297+07:00. GPU checksum unchanged; git diff --check passed.
No gameplay launched for this patch.


## 2026-11-01 — fix Normal item targeting rejection across difficulties

User observes Nightmare withholding items in Normal Arcade/private. Found a
definite adapter bug: shot_in_cone rejected degrees>=90, whereas actual
update.pak kartaisystemdata root458 has cone120/180/240 for Easy/Medium/Hard
(root+20), ranges1000/3000/5000 (+10), radii75/200/400 (+18). Extended profiles
use Hard's240. Therefore every authentic cone was rejected regardless of a
positive native scan. Replace tan half-angle comparison with atan2 of lateral
distance outside the radius versus forward distance; accept finite angles up
to360. Keep forward/range/height checks and native negative decisions.

Also restrict native dc9db0 faction rejection to race settings teamCount>1.
It compares faction via ff1810, while FFA opponents can share a faction. Native
scan normally invokes it only for Battle. Team racing still excludes friends;
FFA keeps native scan eligibility. Guest/Adventure/Battle retain native policy.

Existing native target-lock timing already scales1/.5/.25/.20/.15/.10 seconds
across Easy through Nightmare; restored targeting lets those decisions proceed.
No forced firing or invented items. Tests now use authentic120/180/240 cones
for all six levels, cover same-faction FFA in Arcade/private, and retain enemy,
friend, rearward, finished-racer, native rejection and authority checks.
Actual firing frequency depends on inventory, valid targets and native item
categories; live gameplay still needs verification.

CPU-only build 2026-11-01T21:51:58.636960+07:00; GPU checksum unchanged.
Online USA/EU fixtures and AI Clang ASan/UBSan passed (existing literal-pointer
comparison warning suppressed). git diff --check passed. No gameplay launched.


## 2026-11-01 — preserve shortcut takeoff and reject pickup detours on approaches

User reports Normal Arcade AI missing shortcuts. Latest completed gameplay
214545 shows accepted authored approach branches and seven accepted shortcut
branches, so branch selection alone was not evidence of successful traversal.
Many terrain markers report grounded state3/prepared3/actions01, beginning a
release/press pair from an active drift. The follow-up tick could set hop8 and
then clear it again in exhausted/oversteered drift cleanup. Give a terrain edge
or pending press priority over that cleanup for its one tick. Regression uses
a completed native slide and checks the follow-up press survives.

Normal box pursuit now also checks the racer's native current segment handle+80.
A flagged shortcut or connected authored approach blocks box pursuit, even if
TT recovery left no upcoming-jump intent. Itemless already skips box pursuit.
This preserves priority in both modes, including recovery; it does not promise
all shortcut landings or add artificial speed/reserves. Existing route/hazard
selection and replication remain native. In-game verification required.


## 2026-11-01 — pre-release before shortcut takeoff

Latest private Itemless log221157 uses Nightmare and accepts shortcut branches,
but repeatedly enters recovery around Crash Cove developer samples239..241
(the jump rising from z13 to89 to158). Many terrain inputs still show prepared3
/actions01: release at the takeoff marker, followed by a press one tick later.
The reference already emits a short512 pre-release window; apply_tt_technique
ignored it. Consume that window to release held R1 before the terrain edge and
keep it released until the edge, then press on the edge tick. Exclude that
window from drift cleanup too. Flat drift-hop markers still have2/512 stripped
by guide_hop; no broad early releases, artificial speed or reserve refill.
Regression now checks the pre-release and same-marker press rather than the
old delayed press. The fallback release/press and airborne/off-path cancellation
checks remain. This is an identified timing fix, not proof of successful live
shortcut traversal on every track.


## 2026-11-01 — all normal-race item categories use difficulty policy

Hook native dcd930 (20-byte instruction-aligned prologue) after dcd1f0's
inventory, roulette, grounded and kart-state eligibility checks. dcd4a0 maps
held items through dcd8c0 into categories 0..4: unconditional, position-based,
front target, rear target, and heading-sensitive boost. Previous target cone
changes only addressed two of those categories.

Retain the native tactical result, with difficulty reaction times of
0.60 / 0.45 / 0.30 / 0.20 / 0.15 / 0.10 seconds (Easy through Nightmare).
If native policy keeps declining an eligible held item, allow the native item
button after 6 / 4 / 2.5 / 1.5 / 1 / 0.5 seconds respectively. Track component
and item identity per racer, using the native timer's 8192 ticks/second clock;
reset after a press to preserve a release between repeated uses. No direct
inventory mutation or bypass of roulette/kart eligibility.

Apply to Arcade/Online normal Single/CUP, solo or teams. Itemless, Adventure,
Battle, finished racers and non-owned online AI retain the native decision.
Tests cover all five categories, six difficulties and both game contexts,
tactical opportunities at the reaction boundary, maximum-hold boundaries,
repeat press suppression and excluded modes/authority. Address/undefined
sanitizer checks pass. Live item consumption and race performance remain to
be confirmed from the next gameplay log.

## 2026-11-02 — Developer jump guidance and item presses during slides

Nightmare selects the `developer` TT recording for both Normal and Itemless;
Hard/Very Hard/Extreme select Tropy/Oxide/Velo. The old 200-unit 3D corridor
could hand steering back to native during a jump solely because vertical timing
differed. Around recorded terrain jumps, an already-following AI now uses the
horizontal corridor, retaining the 500-unit height guard and full native recovery
for lateral departures. Centering and pickup exclusion begin 1.2 seconds before
the reference takeoff and cover its landing. Flat drift-entry hops retain the
ordinary corridor. Logs identify the selected recording and distinguish full
3D error from corridor error when recovery changes.

Native dcb6d0 skips dcd3d0 item execution whenever driving component +48 is set
by drift/hop (dcf1c0/dcf740). Thus dcd930 decisions in logs did not prove item
presses. After native input generation, an authority-only adapter puts bit 32
on the existing replicated input when a fresh eligible item decision was skipped
by this gate. It preserves drift/boost/profile bits, requires a release between
presses, and excludes Itemless and Battle. Existing difficulty-dependent reaction
and maximum holding times apply to every item category. No inventory injection.

Validation: runtime tests for fresh versus stale decisions, repeated presses,
Itemless, retained drift/profile; synthetic terrain-height/lateral/bridge cases;
160 local ghost references; complete executable build. Actual shortcut completion
and item consumption still require a race with the new executable.

## 2026-11-02 — ghost owns routing, including rejoining

User explicitly requested removal of native route fallback while retaining
pickups. Latest host log 20261102_000804 confirms Developer selected on Crash
Cove but contains 144 native=1 recovery transitions. This supersedes the earlier
hybrid recovery policy.

For a valid loaded reference, off-path or reverse-facing AI now projects back
onto the ghost instead of returning native targets. Local progress uses a tighter
24-sample forward window; distant/height-separated reacquisition uses the closest
3D ghost segment. Technique alignment still gates hop/drift replay, but does not
release steering ownership. Native target distance and lane-occupancy offsets no
longer influence ghost lookahead. Discontinuous lookahead stops at the last valid
ghost point. Low-speed native spline-recovery and native route hops are cleared
when ghost guidance owns the targets. Native physics, controller output, race
progress bookkeeping and network authority remain intact.

All six difficulties use available references: Tropy for Easy/Medium/Hard,
Oxide for Very Hard, Velo for Extreme, Developer for Nightmare. Normal retains
item/Wumpa pickup detours; Itemless does not seek boxes. Missing/invalid references
still cannot provide a ghost route and retain the existing native behavior.

Validation: guide tests retain ghost steering while distant/reversed and cover
local reference assets. Integrated race_targets test supplies deliberately wrong
native targets plus a recovery request: Arcade/Private Normal targets a box and
returns to the ghost after pickup; Itemless directly targets the ghost. Full AI
and TT test executables and production build pass. No live race with this new
binary yet; clients must relaunch.

## 2026-11-02 — bounded stuck recovery and continuous slide chains

Latest log 20261102_001512 shows all seven AI still requesting throttle at
near-zero normalized speed late in the race, plus active slides losing their
hold when reference alignment drops. Only three accepted slide boosts were
logged before stalls. User requested native recovery specifically when stuck.

Use the tactical component's native simulation timer (+68, virtual +b0,
8192 ticks/second), per owner/lap. Less than 100 units displacement for one
second and speed below 100 activates native recovery. Preserve native targets,
hop and reverse state; bypass custom native branch/line preferences while
recovering. Ghost guidance still tracks progress. Return only after at least
two seconds of recovery, over 250 units displacement, speed above 300 and
0.75 seconds continuously aligned with the ghost. Countdown/disabled driving,
owner changes and lap changes reset the detector. New diagnostic lines report
both transitions.

Flat hop markers are now recognized even without a simultaneous drift-hold
bit; the existing >100-unit recorded climb/drop test still retains terrain
jumps. Higher difficulties keep a legal active slide across reference hold/
alignment gaps until actual completion, oversteer, terrain takeoff or U-turn.
Boost adaptation applies to the active physical slide independently of the
ghost hold bit. Reserve deadline timing also applies in the pre-takeoff release
window; no reserve injection or bypass of native meter validation.

Tests cover countdown exclusion, timing boundaries, delayed return/hysteresis,
owner/lap reset, and integrated native-target fallback/ghost return. Existing
six-difficulty boost and drift-entry tests plus new hold-gap and flat-hop cases
pass, as do all local ghost tests. Live race behavior remains to be retested.

## 2026-11-02 — remove competing steering corrections

Latest host log 20261102_002518 contains 591 technique-recovery transitions,
50 stuck recovery transitions, and 230 sampled forced slide holds. Of those
holds, 180 have absolute route-heading error above .35 (examples .975/.971,
with native turn around -3). Holding every otherwise-legal slide across arbitrary
heading errors was too broad; it could fight the requested route correction.

The TT guide also shortened near lookahead abruptly to 150 units during
rejoining, or 180 units near any hop, compared with up to 450 during normal
travel. At speed 3000 the short target is only 50ms ahead. Hop/technique state
therefore changed steering gain even when route and speed were unchanged.

Remove those state-dependent lookahead lengths. Use a consistent speed-based
quarter-second near target (180..900 units) and half-second far target
(400..1800), retaining arc-length traversal of the recording. When velocity
points forward along the current segment, retain monotonic segment/fraction
progress instead of searching backwards on every frame. Actual reversing and
far spatial reacquisition remain available for native stuck recovery.

Restrict the extra continuation allowance to absolute heading errors <= .35;
larger errors keep native slide release. Commit to a still-valid box approach
instead of allowing peer-occupancy changes to select another box. Itemless and
native stuck recovery behavior remain intact.

Regression tests cover stable lookahead across recovery/hop state changes at
race speed, forward projection jitter versus true reversing, large-angle slide
release and changing peer box claims. AI, TT and local-asset tests pass. These
checks establish decision consistency, not live vehicle stability; a new race
is still needed to verify actual driving with the built executable.

## 2026-11-02 — native perfect window and exact ghost takeoff markers

Latest 20261102_003219 log records 52 accepted slide boosts: 49 first and three
second boosts, with no third boost among those bounded diagnostic samples.
Metadata initializer 018ff0ba registers KartController field index 15,
_perfectDriftBoostThreshold (name 01d172e6), with default float 0x3f4ccccd = .80.
The former expert .92/.95/.97 timing waited well beyond this default window's
start. Set Very Hard/Extreme/Nightmare to .86/.84/.82; Easy/Medium/Hard retain
.80/.84/.88. Native meter, physical slide and boost-count validation remain.
Reserve emergency timing still uses the legal .80 threshold, without modifying
reserve amounts or vehicle speed.

A real terrain-hop preparation can bank a ready boost before releasing drift,
but only when remaining spatial distance leaves a separate release tick. This
uses a two-tick preparation distance (bounded at 240), a bank-once phase, then
release of both drift/boost buttons, then the takeoff press. If too close, release
immediately rather than delaying takeoff for a boost. Flat hop handling and
native safety release remain intact.

Previously the exporter snapped input edges to a nearby movement sample, and
the guide fired edges at the nearest sample's midpoint. Insert an interpolated
position at each original jump timestamp instead, preserving the recording's
exact input time. The guide consumes edges only on crossed samples. Its limit
is now 4096 samples and 512 bytes of consumed-hop bits to accommodate insertion.
These are spatial triggers at the runtime simulation's tick resolution, not a
claim of sub-frame synchronization or guaranteed successful jumps.

Validation: seven Python tests including all local native assets, exact inserted
hop time/position, runtime pre-takeoff bank/release/press order, late-bank rejection,
no premature marker crossing, existing difficulty/reserve gates and full 160-track
C reference checks pass. Regenerated out/tt-reference/reference.bin. Production
build prepared; actual perfect-chain count and fire continuity need a live race.

## 2026-11-02 — adapt the recorded rhythm to the actual kart

User corrected the objective: imitate the ghost's driving style dynamically;
perfect quality and completing three boosts are not universal objectives.
Removed the difficulty-specific perfect-meter threshold array added previously.
Difficulty still selects the reference tier and existing driving/item policy.

Export exact boost-button and start_drift edges as well as hop edges. The new
8192 start marker remains internal recording data; the runtime intent mask does
not pass it as an action. For the local recorded hold, measure each boost's
interval from actual recorded drift entry or the preceding boost. Match it to
the actual kart's completed-boost count, rather than copying a future ghost
button pulse or making every boost perfect.

Translate this interval into the actual kart's meter period from its native
archetype drift data (+a8, period +20). Read the charging timer (+f8, 8192 Hz)
and the actual fastest-fire reserves. Cap the planned wait at the reserve
expiry minus one simulation tick; for imminent hop preparation, bank the meter
already available. Keep native eligibility/physics and cap below meter expiry.
When a reference cycle or runtime clock/data is unavailable, use the native
caller's timing; do not synthesize reserves or vehicle speed.

Validation: a recorded .3/.6/.4-second three-boost fixture drives three distinct
native threshold requests, rather than a common quality target. Runtime tests
also cover actual charge plus a .2-second reserve deadline yielding a below-
perfect request, imminent-hop banking and native rejection. Seven exporter tests,
all 160 regenerated references and runtime reference tests pass. This validates
the decision path; driving stability, consumed boosts, fire continuity and
successful shortcut traversal still require a live race with the new build.


## 2026-11-02: empty boost consumption and pad reserve accounting

Evidence: host log `20261102_010056.log` contains repeated slide releases and
opposite-direction re-entry within the same recorded hold, seven sampled infinite
reserve readings, and consumed boost counts with near-zero fire. Counts alone do
not establish that turbo was produced. Native `e32850` increments kart+ec before
checking normalized charge against archetype+a8 drift-data+24; below that minimum
it never calls e2dd10 to create turbo. Kart+c4 is the separate Perfect threshold.
The previous emergency/pre-hop timing could therefore consume an empty boost.

Changes:
- Clamp guided timing to the actual drift-data+24 minimum after applying ghost
  intervals and reserve/takeoff deadlines. Invalid minimum retains caller timing.
  No universal Perfect threshold, synthetic speed, or reserve refill.
- Native f4b770/e2e6b0 calculate finite carry from maintained_duration + data+1c
  + entry+28 - entry+18, even on a maintained pad. Use that calculation rather
  than reporting entry+30/+31 as infinity. e75750 confirms those flags affect
  maintenance/ageing, not an infinite bank after leaving the pad.
- For guided upper difficulties, reject slide entry whose native heading sign
  would produce the opposite steering direction to the recorded slide. Lane
  corrections must not start alternating opposite slides within one ghost hold.
- When native heading correction releases an active slide, bank a sufficiently
  charged boost for one tick first. Previous-input edge checks prevent repeated
  banking; native complete/oversteer and terrain takeoff remain authoritative.
- Rename count diagnostics to consumed slide boost; a counter increment is not
  proof of turbo creation.

Validation: C race-options tests include finite pad/post-pad carry, expiring-fire
and pre-hop minimums across all six levels in Arcade/Private, both recorded turn
directions, one-tick bank/release and oversteer veto. C reference tests and all
seven Python ghost-export tests passed. Production build uses existing GPU build.
No clients were running during this work. Shortcut success and sustained fire
across full live races at all difficulties remain unverified; particularly the
latest log also contains prolonged native fallback stalls, which these tests do
not establish as resolved.


## 2026-11-02: shortcut recovery shared by all tracks

Latest host log 20261102_011551 contains 13 stuck entries and 13 native-to-ghost
returns. Recovery eventually moves every recorded case, but stalls recur. Some
recorded takeoffs are materially slower than the reference (2126.2 vs 4269.0).
The old recovery gate only reacts after low-speed stagnation; horizontal alignment
can also permit rejoining while still inside a terrain-jump window. The old log
has no positions/heights, so it cannot prove which stalls are low landings.

The shared guide now emits BELOW_ROUTE when the projected ghost trajectory is
more than 200 world units above the kart inside a real terrain-jump window.
Runtime validates the attached kart and grounded contact; recovery requires a
continuous quarter-second of this condition. Airborne height/timing differences
therefore do not trigger this path. This permits recovery before a failed landing
comes to a complete stop. Existing one-second stagnation recovery remains for
other obstructions. Native recovery must leave the terrain-jump window before
returning to ghost guidance, in addition to existing movement/alignment hysteresis.
No map names, per-track coordinates, teleportation or boost grants were added.
Transition logs now include lap/position and native recovery elapsed time.

Expanded real-asset regression from Crash Cove to all 160 references: 40 tracks,
all six difficulty selections, seven race laps, 212933 sampled poses across 240
track/difficulty combinations. Each recorded trajectory stays guidable, produces
finite targets, is never falsely flagged BELOW_ROUTE at the sampled poses, and
has safe moving/aligned positions outside terrain windows for rejoining. Synthetic
cases cover above/below geometry, continuous versus transient low landings,
recovery while still moving and holding native control through the jump window.
Both C test suites pass. This is controller/reference verification, not a claim
that all shortcuts have been driven successfully in the live game. Faster and
more accurate shortcut approaches still require gameplay evidence.


## 2026-11-02: long native fallback and Normal pickup starvation

Host log 20261102_012253 reports native recovery spans up to 27.51 seconds,
including BELOW_ROUTE entries while moving at 2524–2614 units/sec. This falsifies
the previous assumption that grounded vertical separation alone means a failed
landing. It also records only 14 pickup target selections, all item boxes (no
Wumpa target), and nine box-target samples among 2763 sampled control records.
Target selection is not evidence that a box was actually collected.

Corrections shared by all maps:
- BELOW_ROUTE recovery now also requires speed below 300; normal fast grounded
  hill travel cannot trigger that entry. Ordinary stagnation recovery is retained.
- A new REJOIN_READY intent requires actual 3D distance below 120, velocity
  alignment and no imminent hop or low-route mismatch. Recovery retains its
  movement/time hysteresis but no longer waits for the broad terrain window to
  end. This avoids the broad-window native latch introduced by the last change.
- Clear old-lap fallback before calling the native target selector, so even the
  first update on a new lap cannot inherit old recovery branch policy.
- Normal pickup planning is allowed during an active slide. Its detour veto now
  covers the immediate recorded takeoff window (-.2/+ .45 reference seconds),
  actual airborne state, hop preparation and low-route mismatch; the wider
  terrain corridor still protects ghost alignment. Flat slide-entry markers do
  not veto grounded pickup planning. Itemless still exits before target changes.
- A retained box can remain targeted within 120 lateral units near contact,
  instead of being rejected by the shrinking forward cone. Behind/vertical/wide
  boxes remain excluded. Added bounded scan diagnostics for live and reachable
  item/Wumpa boxes and selected category.

Native crate-list investigation corrected an intermediate suspicion: manager+10
IS the all-crates list, not an item-only list. fdd570 removes content0 from+18,
content1 from+20 and either from+10. Reflection names are _crates,
_powerUpCrates, _wumpaCrates. No manager-offset change was made.

Validation: race-options tests cover fast grounded travel, safe 3D rejoin inside
broad terrain, first-update new-lap reset, both box categories during drift,
close-contact retention, airborne/takeoff/Itemless exclusions. All 160 references
again pass 240 track/difficulty combinations, seven laps, 212933 sampled poses;
every combination has REJOIN_READY positions. Runtime shortcut completion rates,
actual pickup counts and fire continuity still require a fresh live race.


## 2026-11-02: superseding Normal / Itemless driving policy

User explicitly replaced the previous all-modes ghost requirement. Normal Arcade
and Private now retain native tactical targets and native branch/line selection.
Ghost guidance/recovery/technique is only invoked in actual Private Itemless.
Normal still uses its pickup planner on the native route and native item-state
validation. Hard/Very Hard/Extreme/Nightmare maximum item-hold decisions are now
1.5/.9/.6/.3 seconds, across all five native categories; native activation gates
and button release edges still apply. Public online does not get the new item
aggression policy.

Base speed is now deliberately increased, superseding the earlier no-speed-
changes approach: Normal Easy/Medium retain baseline; Hard +10%, Very Hard +20%,
Extreme +30%, Nightmare +40%. Itemless uses the same scale with Medium +5%.
This is a base-speed parameter change, not a guaranteed measured lap-time gain.

Verified native path: e4a220(kart, speedData, kind, speed, dt) reads speedData+c
and its curve pointers +18/+20/+28. The wrapper passes a call-local 0x30-byte view
with a scaled base-speed field. Shared archetypes are unchanged; repeated calls
cannot accumulate multipliers. AI-only eligibility is established by native AI
input components and attached kart/entity identity; the registry clears on map
loading. Both owner and replicated-AI physics use the selected network profile.
Human karts, Adventure, Battle and public online preserve native behavior.

Private Itemless speed-modifier hook e452c0 first runs native expiry/timer logic,
then retains the product of positive modifiers and omits the strongest negative
slowdown (matching the native ignore-slow policy). It restores the previous
kart+194 slow-surface timestamp so post-surface damping is not newly armed.
Kart+198 damage deceleration, brakes, collision handling and positive boosts are
not overwritten. This targets native max-speed slowdown modifiers; it is not a
velocity overwrite or teleport.

Itemless traffic response offsets both guide targets by at most 90 world units
on straight reference sections when another racer is nearby on the same height.
The offset varies continuously with relative position and has stable racer-ID
side selection. No offset is added on bends, terrain hops or with no nearby
traffic. This is local passing separation; it does not claim collision-free
navigation on every possible road edge.

Native hooks: e452c0 and e4a220 have verified identical 17-byte prologues in the
USA ELF and EU text image. Two trampolines extend the allocation from448 to512
bytes; neither copied prologue contains a relative instruction. Integer/float
argument registers were checked against disassembly. Modifier reflection offsets
were verified from CKartController metadata at265afc0 /1da4570.

Validation: race-options C tests cover both Normal modes at Hard through
Nightmare, Private-only ghost guidance, native branch selection, all item
categories, non-accumulating six-level speed, player/public/Battle exclusions,
replicated AI, positive/negative surface modifiers and timestamps, map registry
reset, and traffic/height/corner/hop separation. Reference tests still pass all
160 recordings, 240 track/difficulty combinations, seven laps and212933 poses.
Production build compiled with warnings as errors. Live driving, passing behavior
and actual shortcut completion remain unverified until clients run this build.


## 2026-11-02: bounded dynamic base-speed bonus

Supersedes the fixed +10/+20/+30/+40% table above. Normal and Private Itemless
Hard now vary +3–5%, Very Hard +5–7%, Extreme +7–9%, Nightmare +9–12%.
Itemless Medium varies +1–2%; Normal Medium and both Easy modes remain native.
A sine wave completes one cycle in 16 simulation seconds. Network racer ID
provides a different phase per AI; no wall clock, random per-frame jumps, or
lap reset is used. The native kart timer's simulation-time method supplies ticks.
The period divides the uint32 tick range, avoiding a discontinuity at clock wrap.
Without a timer the bonus stays at its phase value rather than advancing by
physics-call count (native calls the speed helper multiple times per update).

Additional native evidence: e46250 adds boostComponent+38 to the base-speed
result, then clamps boosted speed using boostComponent+4c/+50. Raising only
the base by 40% can conflict with this separate native boosted-speed range.
The new maximum base increase is 12%; boost limits, boost contribution and
reserves are not scaled, bypassed or replaced with a forced velocity floor.
The latest trace also shows speed losses alongside reserves reaching zero;
it does not establish that every reported fire-speed loss has the same cause.

Validation: controller tests pass, including all difficulty bounds, full-cycle
variation, per-racer phase differences, tiny adjacent-sample changes, period and
uint32 wrap continuity, actual timer wiring, non-accumulation and eligibility.
Native boosted-speed clamp behavior is verified from decompilation; live red/
blue-fire transitions and the subjective race balance still require gameplay
with the rebuilt executable. No track-specific changes were introduced.


## 2026-11-02: dynamic-speed AI registration crash

The 02:19:24 session crashed while spawning AI before the race. Host offsets
+0x41e98/+0x454f3 resolve (non-PIE base 0x400000) to race_handle and
register_ai_physics, called by ai_input. The dynamic-speed identity lookup
mistook AI component +28 for a racer handle. That offset belongs to the steering
target vector; +28 is a racer handle on the separate tactical component only.
Nonzero target coordinates became a noncanonical pointer and caused SIGSEGV.

Registration now matches the validated kart entity against the existing native
racer-manager list, using the same racer entity/player layout as traffic handling.
Missing manager/identity safely uses phase zero. No target coordinates are
interpreted as pointers. The initial synthetic fixture had zeroed steering
targets, which concealed this error. Populating them with real-shaped nonzero
coordinates reproduced exit 139 before the fix. The corrected fixture covers
manager absence, matching entity, native and player IDs, peer execution,
unmatched entities and malformed count/ID bounds, alongside dynamic speed tests.
Live race startup still needs verification with the corrected executable.


## 2026-11-02: exclude public matchmaking from all custom AI difficulty

Custom AI eligibility is now Arcade or confirmed Private only. This supersedes
historical sections that included public OnlinePlay. Selected driving level is
inactive outside that boundary and effective extended profile is zero. Public
reads the native authority/follower difficulty field without the cached private
profile or lobby-personal override. Its AI input calls native directly without
publishing or adopting the custom profile bits, pickup/ghost adaptation or extra
item presses. Public entry restores cached tuning before native input runs; the
AI-data getter also restores all three original columns outside eligible modes.
This covers start boosts, hazard avoidance, lock delays and catch-up parameters,
not only the existing private-only bonus-speed and item-hold guards. A reused
selector removes appended difficulty entries and clamps stale extended cursors
back into the supported native range. Arcade/Private preference remains saved
locally for the next eligible race.

Controller regression checks include repeated Private -> Public -> Private
transitions, byte-preserved original Easy/Medium/Hard tuning columns, public
host/follower input and profile isolation, native difficulty ownership reads,
reused selector cleanup, public native item decisions across all five categories,
and no injected item button during a public slide. Dynamic speed and the prior
nonzero-target registration crash regression remain in the same test suite.


## 2026-11-02: four-track Private CUP difficulty regression

Reviewed the retained private-CUP flag, map-loading hook and room-exit reset.
Track-to-track loads retain followed base/profile while clearing old AI physics
registrations; only lobby boundaries/room exit clear the followed selection.
Extended the room-difficulty test across four consecutive track loads for every
pair of personal and host levels (6 x 6), including recreated wire state and
owner/follower transitions on every track. Host difficulty remains selected and
the personal field stays unchanged; the existing lobby-return assertions verify
restoration afterwards. No production behavior change was needed.
Race-options C suite passes with warnings as errors. This is synthetic lifecycle
coverage, not a fresh live four-race host/guest gameplay confirmation.


## 2026-11-02: Itemless Easy/Medium remain native

Per the final user correction, this exception applies only to Itemless. Easy
and Medium bypass custom driving hooks, ghost guidance, base-speed bonuses
and surface slowdown removal. Their original tuning columns are preserved,
including after switching from Normal; the tuning cache distinguishes both
policies. Normal behavior is unchanged. Hard uses N. Tropy, Very Hard Oxide,
Extreme Velo and Nightmare Developer references in Private Itemless/CUP.
Controller regression covers mode transitions, all six levels, original lower
tuning columns, Medium speed and surface modifiers. Live gameplay is unverified.
