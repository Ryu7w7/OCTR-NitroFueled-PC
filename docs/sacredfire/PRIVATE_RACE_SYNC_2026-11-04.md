# Private Team / CUP follow-up, 2026-11-04

The local `20261104_173827.log` records a 3481.2 ms frame when the host
starts Team selection. There are no pipeline compiles in that frame, capture
wait is 1.8 ms, and the last GPU command buffer takes 6.2 ms. Two consecutive
party checks precede selector initialization. This establishes a game-thread
stall, but does not separate HTTP time from native selector preparation.

The same log records a CUP guest adopting Normal, and host mode publications
finishing out of selection order. Team results already contain host-first
ordering before the panel renders; one race assigns zero points to everyone.
Changing only scoreboard labels cannot repair that underlying state.

## Changes

- Serialize room-mode writes and coalesce pending choices. A delayed Normal
  write can no longer overwrite the last Itemless choice. Add a concurrent
  test holding the first request while twenty more selections arrive.
- Private Play waits for a successful publication without blocking on another
  publish request. Before track loading, both clients read the room mode;
  the host also checks it against its choice. Missing, malformed or failed
  verification cancels the transition through the native lobby-return path.
  The verified mode stays fixed through the race and all CUP tracks. Polls
  cannot overwrite it until returning to the lobby. This uses the existing
  room-mode endpoint; no native packet format is changed.
- Guest selector initialization retains the adopted host mode instead of
  resetting it to the guest's personal preference. Lobby polling checks the
  actual applied mode, so an incidental local reset is repaired. A repeated
  session-info reply for the same host no longer restarts following or clears
  the race snapshot. Replies from rooms already left are discarded under the
  same lock that applies the mode.
- Retain the Itemless archive view during a native private session even if
  peer packets are quiet during loading. Broadcast race settings for CUP as
  well as Single.
- Reuse an approved party check only within the same native button dispatch,
  and only for the identical account, feature and roster. Later Start events
  still validate again. This removes duplicate blocking work, not all HTTP
  work from the game thread.
- Match AI result IDs through the native network roster (spawn slot at +5c,
  network ID at +58), as native result handling does. Reapply Team scores after
  native results and late finish callbacks so newly completed racers can
  receive their points. The first Team score/effects calculation now runs
  after native result processing, not before remote finish flags settle.
  DNF still requires the configured lap count and a
  valid final time; no synthetic finish flags or human placements are added.
- Native live race comparator `ff11a0` (EU -0x10) compares laps, checkpoints,
  and spline distance but has no final ordering for equal progress. Its callers
  are the native sorting routines at `eae8xx`–`eaf0xx`. Record the validated
  native starting grid after `f31160` placement, which writes racer +58.
  For private Single/Team/CUP only, break native ties by that grid, keyed by
  network identity. Native progress always wins when either racer is ahead.
  Missing/duplicate IDs or invalid grid positions disable this tie-break;
  map transitions clear it. This addresses equal-progress reshuffling without
  rewriting lap progress, finish flags, or positions in a GUI callback.
- Increase Itemless AI tuning for Hard through Nightmare. Base speed centers
  become +8%, +14%, +20%, +26.5%, retaining existing identity variation.
  Airborne speed receives another 10–19%, normal hop impulse 20–26%, and native boost
  speed contribution/caps 12–21%, including sacred fire. Per-call boost values
  are restored, so boosts do not compound or change reserve duration. Human
  physics and Normal tuning remain unchanged. This interprets the requested
  difficulty change as AI tuning.
- Trick-jump impulse `e33d80` reads archetype +170 and adds +174 when kart +e8
  is set; the normal takeoff function selects this path at `e497a1`. Only for
  registered Itemless AI, evaluate that native slope-adjusted trick impulse
  with the added-impulse flag and another 8–14%, restoring the flag afterward.
  This uses native trick-jump rules, not a claim of measured perfect-jump
  height or success on every shortcut. Native takeoff eligibility is retained.
  An additional live-racer human-handle check rejects human karts even if an
  obsolete AI registration points at the same entity.
- Online AI speed variation depends on racer identity rather than a local
  kart timer, so different client loading/clock phases cannot select different
  speed multipliers. Offline retains its time-varying pacing.

## Validation and remaining uncertainty

Online-mode, race-options, public-Itemless, room-mode concurrency and archive
predicate tests pass. Native motion hook signatures pass against both local
USA and EU 1.21 dumps. The application builds successfully.

Regression cases also cover host publication failure/pending state, a server
mode differing from the host, invalid guest replies, cancelled loading before
opening track assets, mode changes arriving during CUP, leaving during a mode
request, repeated session-info responses, guest selector reset, different AI
clock phases, guest-first/host-DNF results viewed as either local player, and
deferring winner effects until native result processing. These exercise the
native hook entry points with synthetic USA/EU memory fixtures, not live races.
New fixtures test all 64 pairwise equal-progress grid comparisons, real progress
overriding grid order, unchanged human trick flags/impulses, and human racers
with stale AI registrations receiving no speed, hop, trick or boost bonus.

The full Python suite ran 413 tests, with 65 skipped and nine `test_probe`
failures also present in `out/pr-probe-baseline.log`. `build.sh --test` passes
the changed component tests but stops later on existing GCC truncation
warnings promoted to errors in runtime-services test compilation.

No two-client gameplay reproduction has been completed with this patch.
The reported live human HUD ordering/progress desync and complete disappearance
of the selector hang remain unverified. The fixes close reproduced mode-state
and result-ordering errors; the log alone does not prove every cause of a
network desync. Both clients must run the updated build to validate Team
guest-first finishes, DNF scoring, and all four Itemless CUP transitions.

### Separate AI catch-up bonus: implemented

`runtime_ai_catchup.c` provides the separate catch-up multiplier. All six
levels apply in both Normal and Itemless. The native AI owner selects the
nearest unfinished human ahead using the game's racer comparison (lap,
checkpoint, spline progress), then measures planar world-space separation
and both karts' horizontal velocities. This is a spatial gap estimate, not
integrated road distance around corners. Human host, guest and split-screen
karts are excluded through their live player handles; unidentified and
finished racers receive no catch-up bonus.

The target speed is the human's speed plus a distance / closing-time margin.
Closing times, Easy through Nightmare, are 18/14/11/8/6/4 seconds; the margin
is capped at 12/22/38/60/90/120 percent of the AI's estimated unassisted speed.
Matching the human's speed is separate from that difficulty-dependent margin.
The overall bonus is bounded to +250 percent, rises by at most 35 percentage
points per second and falls by 65. After passing one human the AI targets the
next; after passing all humans the catch-up component fades to zero. Existing
base, Itemless airborne and fire improvements remain separate. The catch-up
factor affects AI base speed and active boost contributions without mutating
shared archetypes or adding a player jump/boost modifier.

Native input metadata research established `_bitfield` as a one-byte
igCharMetaField (factory 0x1545320, initializer 0x1bbd832, name 0x1d903b3).
Its action/profile bits and adjacent padding are unchanged. The bonus instead
uses 48-byte RYUPN1 control messages, kinds 3/4, on the existing game socket's
direct/relay path. Older ping implementations discard these unknown kinds.
There is no extra HTTP request or blocking wait in the simulation loop.

Only the host's native-owned AI computes the bonus online. Every endpoint
uses the same quantized percentage, indexed by the native AI spawn slot,
which must not be confused with a human network ID. Followers accept snapshots
only from the current room host's known, recently contacted peer endpoint.
Map hashes, an echoed per-race follower nonce and increasing sequence numbers
reject other tracks, repeated-track stale data and reordered/duplicate packets.
Guest snapshots expire after 1.5 seconds. A host with remote human racers
requires current handshakes from its observed peers before enabling bonuses;
unsupported/disconnected transport suppresses the bonus instead of causing
independent guest calculations. Updates are limited to 20 per second per peer.
The normal build's RyuNET ping/tunnel transport is required for online catch-up;
if disabled, multiplayer catch-up stays inactive. Offline/local AI does not
need that transport.

Tests cover both modes and all difficulties, nearest-human target switching,
return to baseline, finished racers, humans with recycled AI registrations,
follower non-recomputation, speed/distance response, packet quantization,
wrong host, malformed length, rate limits, old races, ordering, expiry and an
incompatible peer suppressing the authoritative bonus. The new tests and
existing changed component tests pass. The final build used
`BB_SKIP_GPU=1 bash build.sh --test`; it produced `out/sacredfire`, then the
broader runtime test compilation encountered the pre-existing GCC truncation
warnings described above. The unrestricted Python suite ran 413 tests with
65 skips and the same nine baseline probe failures, no additional errors.

The prebuilt includes catch-up now. Live host/guest gameplay and tuning on
shortcut-heavy tracks are still unverified; asynchronous network delivery
remains subject to the game's ordinary prediction/correction latency. These
unit tests are not a claim that every possible race desync has been eliminated.

### Guest Cup standings subtitle

The native Cup ranking asset has distinct `[CupName]` and `[STANDINGS]`
placeables. Its graph consumes `CGuiEventRaceEndUpdateCupData::_cupName`,
separate from the synchronized standings/results. The reported guest panel
can therefore lack its subtitle even though its Cup scores are populated.
After native events on the active private Cup ranking project, refresh only
`[CupName]` from the adopted Cup's localized name (`cup+0x30`). The traversal
also handles nested compositions. Native scoring, standings title, animations
and other ranking projects retain their existing behavior.

The USA/EU synthetic hook tests cover an initially empty subtitle, a later
stale native subtitle, and preservation of STANDINGS across all four tracks.
They pass; the actual two-client panel rendering still needs confirmation.
The prebuilt was rebuilt with `BB_SKIP_GPU=1`.

### Itemless catch-up fire and airborne follow-up

The latest host log (20261104_194941) reports only ten catch-up entries, all
at 1%, and profile changes between Hard and Nightmare. Two code paths allow
this: catch-up was gated on native AI ownership as well as room authority,
and a room host could adopt a stale profile from a non-owned AI input.
The room host now computes every AI's bonus regardless of native ownership;
native ownership still controls AI actions. Snapshot byte 41 carries the
host difficulty level (plus one; zero means unavailable), so guests can use
that profile even after ownership moves. The bonus ramp rises at 75 percentage
points/second and falls at 100; the distance/speed target and difficulty caps
remain separate from the existing base speed bonus. Logs now include readiness
and native ownership even when the bonus is zero.

Native CKartEffectExhaust getter e8ad70 (EU -10) resolves its parent handle
at +18; parent effects component +18 identifies the entity. Exhaust update
e8a360 computes the owner's state and copies the applied state to +10, while
followers consume +10. The hook caches actual Fire3/Fire4 states (8..11) and
uses the latest tier during Itemless AI catch-up. Native effects replication
carries the display. An ownership takeover can seed its cache from the
previously replicated current state. No fire is invented before acquisition;
underwater effects, human racers and Normal-mode effects remain native.

Disassembly of e46250 confirms its second argument is the entity and its
final velocity is applied through bb5060. Itemless Hard+ AI now records a
horizontal takeoff speed floor with a one-time 10–16% margin, for at most
three seconds. It preserves the native horizontal heading and vertical
velocity, using the native velocity setter so changes are replicated. Landing
clears the floor; a speed loss exceeding 25% cancels it to avoid reviving a
stopped/crashed kart. It does not redirect AI through walls or eliminate
native collision/respawn rules. Higher jumps alone do not ensure correct
shortcut alignment, so live track testing is still required.

Focused tests cover migrated ownership, host profile protection, host level
transport, latest red/blue state, no acquired fire, underwater/Normal/human
exclusion, fixed airborne speed without compounding and collision cancellation.
Hook prefixes pass against local USA and EU 1.21 dumps. Runtime build uses
BB_SKIP_GPU=1; no GPU rebuild. Live two-client gameplay remains unverified.

### Cortex Castle jump failures: reference approach speed

Host log 20261104_202418 shows terrain takeoffs substantially below the TT
reference speed (AI 4: 1933.6 versus 4460.1), plus repeated route recovery.
A percentage of that insufficient takeoff speed cannot supply the required
horizontal range. Separately, almost every catch-up report has ready=0;
this change does not claim to repair that transport failure.

TT guidance now exposes the upcoming non-flat terrain jump's horizontal speed
within 1.2 recorded seconds, only while aligned with the route. Itemless Hard+
AI accelerates toward it on the ground at up to 6000 native units/s², bounded
to 50 ms per update; pickup detours are blocked during that approach. The
native velocity setter preserves heading and vertical velocity. Native owner
alone applies this assistance online; followers receive native movement.
Humans, Normal mode, flat drift hops, reversing/off-route and stopped karts
are excluded. Recorded approaches keep actual takeoff speed in flight without
stacking another percentage on the reference speed. Existing collision and
three-second airborne cancellation remain.

The local search previously rejected a ghost segment when the AI was more
than 500 units above it, potentially reacquiring an unrelated section of track.
In an established terrain-jump corridor, local projection now uses horizontal
position when above the recording (up to 1500 units); below-track and global
reacquisition keep their height checks. Recovery can return to that horizontal
corridor without waiting for matching ghost height. Takeoff diagnostics now
report actual and reference speeds, capped to once per second per AI.

Focused TT tests cover upcoming terrain speed, flat/reversing exclusion,
owner identity, and a higher airborne trajectory retaining local steering
without replaying its jump. Integrated physics tests cover acceleration via
the real reference reader and native setter, and human/Normal/follower
exclusion. Both suites pass. CPU-only prebuilt rebuilt; actual two-client
Cortex Castle shortcut success still needs gameplay confirmation.

### Direct LAN catch-up transport

The ready=0 cause in the Cortex Castle session is the transport filter:
ping_after_send used tun_candidate, which intentionally rejects 192.168/16
and other local addresses for relay traffic. Both clients were exchanging
native packets directly on 192.168.100.9, so neither obtained a ping identity
or catch-up handshake on that endpoint. Tunnel startup itself succeeded.

Direct local game destinations now discover ping identity during an active
private catch-up session (host requires remote peers, or guest follows a host).
Server/relay destinations stay excluded. LAN control packets go directly;
the existing tunnel filter still prevents relaying them. After identity
exchange, the existing host identity, map, race nonce, sequence ordering and
expiry checks govern snapshots. A peer must still complete the catch-up
handshake; merely answering a ping does not enable its bonus. No HTTP request
or retry loop is added to game dispatch.

A two-process UDP loopback test exercises actual ping_after_send/ping_take,
identity discovery, nonce handshake, host bonus publication and guest profile
adoption. Both processes observe +42% and the guest adopts level 5. This test
passed outside the socket-restricted sandbox and is included in build.sh
--test. Existing ping and catch-up unit suites also pass. Bounded logs now
confirm the first guest handshake and host snapshot in each race. This fixes
the identified LAN transport failure; live gameplay still must confirm the
combined jump/catch-up behavior on my track.

### Cup race-two disconnect and remaining HTTP stalls (21:15)

Latest host log `20261104_204153.log` advances Mystery Caves -> Papu's
Pyramid at lines 18023–18024. Guest `20261104_204155.log` receives race-one
results but never loads track two, then drops the host at line 14010.
Native inspection identifies two different end-screen paths: host eac310
was redirected to Cup advancement, while guest f39630 still asks to return
to the lobby and schedules f399a0, which disconnects the host session.

Private Cup now redirects the guest's first countdown callback to the
native remaining-grace timer f398a0, with the same Cup advancement callback
as the host. It omits the single-race lobby request/disconnect handshake.
The native countdown still originates from the host's reliable
CMatchFinishedNetworkEvent; no HTTP or independent wall-clock timer is used.
Non-Cup guests retain the original callback through a relocated trampoline.
Fixtures exercise all four tracks, guest remaining-timer scheduling, and
non-Cup fallback for USA/EU. Native hook bytes and timer field access are
checked against both installed 1.21 dumps. Real two-client transitions have
not yet been played with this build.

A separate native completion shortcut at f344e6 returns finished when all
humans finish, disregarding unfinished AI. Private Single/Cup now bypass
that shortcut so native race timeout can run. The wrapper retains immediate
completion when all participants finish. Public/Battle/offline restore the
original branch. Tests check partial/full completion and patch restoration;
this addresses premature race cutoff, not artificial completion of AI that
miss checkpoints or are still racing at timeout.

Compatibility roster checks (public/private) now run in a background worker
with a five-second cache keyed by exact roster and requested feature. Lobby
updates prefetch; an uncached Play remains fail-closed and asks for a retry
while verification is pending. Final room-mode verification also runs in a
worker; the main-thread lobby update resumes the queued map load. Mode,
publication and room generation are checked before loading; ending a load
invalidates its verification epoch. Existing Cup mode freezing remains.
Tests deliberately block HTTP GET while repeatedly invoking the frame-side
checks, then release the response and verify completion. The native lobby
fixture also checks that updates continue while a map verification is pending.

Focused online-mode, room-mode, public-itemless and AI suites pass. USA/EU
native contract tests pass. `BB_SKIP_GPU=1 bash build.sh` succeeds and retains
the existing GPU library. Live hangs, AI finish timing and four-race Cup
session continuity still require testing on both clients using this prebuilt.

### Drive-Thru Danger and recorded Cup AI finishes

The same 20:41 host session subsequently loads `t508_toxic` (Drive-Thru
Danger). It contains 34 recovery entry/exit records, including confirmed
below-route landings, stops, and long native recovery periods. It predates
the 21:13 prebuilt. Mystery Caves in that session reports all six AI entering
lap 4 before the Cup result event. The old log lacks final lap-list/time/flag
snapshots, so it cannot prove exactly which finish field the panel lost.

Terrain hops now anticipate the next 30 Hz simulation step instead of waiting
until the kart has passed the takeoff edge. Crossed hop markers are consumed
across the complete local projection window, not just four samples. Flat
slide hops and nonlocal reacquisition retain their previous policy; markers
remain single-use per lap. Airborne speed support no longer pins catch-up
overspeed above a known recorded jump speed, or stacks a second multiplier
on unguided takeoff. Native slowdown, gravity, and velocity replication remain.

The native lap recorder f350c0 is now wrapped for private Cup AI owned by this
simulation. After the original recorder, completion requires local/native
ownership, no human player handle, exactly the configured lap count, a final
crossing counter of laps+1, and positive increasing native cumulative times.
It then calls native f351b0 completion (including the existing finish event
and race bookkeeping), never fabricating times or writing finish flags.
Already-finished racers and incomplete/invalid records are ignored. This
repairs the missing-completion case when genuine lap records exist; it does
not treat lap=4 alone as proof of a valid finish. First native finish callbacks
and host result application now log their lap/time/flag snapshots for both
clients, so any separate replication or UI issue can be distinguished.

Tests cover anticipating a cliff hop without replay, fast local projection,
no airborne overspeed lock, invalid/incomplete times, human/follower exclusion,
repeat completion, and unchanged recorded times. Focused TT, AI and online
suites pass; the new native hook prefixes match both USA/EU dumps. Live
Drive-Thru Danger landing success and both clients' Cup finish panels still
need gameplay verification; no claim of zero falls/desync is made.

### 21:32 session: recovery physics and final-time replication

Host `20261104_213231.log` and guest `20261104_213233.log` ran the new
Drive-Thru Danger build. Host recorded 28 recovery transitions, with one
recovery lasting 48.78 seconds. AI 6 and 4 completed three valid laps; AI
7/5/3 still had two records and AI 2 only one when the timeout ended.
Those four DNFs are actual incomplete laps, not lost completion records.

The logs expose a distinct results bug: identical cumulative lap timestamps
on both peers, but guest final_time differs for AI 6 and the host player,
and is zero for AI 4. Cup results now derive final_time and completed-lap
count from the validated native cumulative lap list. This also runs before
Cup panel events to cover a delayed lap update or callback after the host
results packet. Invalid/incomplete records do not get a fabricated time.
Tests exercise clock skew, zero final time, real DNF, repeated results and
late panel updates.

During native recovery the kart was still receiving ghost-route base speed,
boost, jump and airborne assistance, despite following native recovery
splines. These modifiers now suspend during recovery and stale airborne
floors clear on recovery entry. In network mode the gate uses an authority
recovery mask sent in byte 42 of the existing catch-up snapshot; both peers
use the same mask, with existing map/nonce/host/sequence/expiry checks.
Guest publication cannot override it. Migration away from host ownership
clears that host decision rather than applying stale recovery state.
Offline mode consults its local entity-bound recovery state. Normal mode
retains its existing behavior. Both clients need the current binary.

Focused AI physics, catch-up protocol and online-mode fixtures pass. The
two-process local UDP test also verifies transmission of the recovery bit
alongside bonus and difficulty. CPU-only prebuilt succeeds with GPU skipped.
Actual landing/recovery success and corrected finish panels still need a
new gameplay session; these tests are not proof that every shortcut is safe.


## Human kart isolation after jump-distance report

The physics registration previously accepted a cached AI component when no
matching live racer could be found. It also treated an unresolved player
handle as an AI. The shared physics gate now requires a current matching
entity, no player handle at all, and an in-range AI ID matching the cached
registration. Missing lists, recycled identities and unresolved human
handles receive native physics. Registration clears cached assistance when
identity changes or is no longer valid. Race-target and item decisions also
exclude human handles, including autopilot; the AI-only Cup completion hook
uses the same raw-handle exclusion.

Regression fixtures now provide real AI racer identities instead of
expecting bonuses without a racer list. Tests cover base/air speed, hop,
trick jump, boost contribution, surface modifiers, catch-up and approach
velocity for resolved and unresolved humans, missing lists, mismatched
entities and invalid/changed IDs. Both peer-decision states are covered.
The report itself has not been reproduced in a new live gameplay session;
the latest saved host log available was still 20261104_213231.log.


## Team selector transition and jump overspeed (22:40 session)

Host 20261104_224040.log entered Mystery Caves after team selection; guest
20261104_224042.log remained in the selector. Deferred mode verification
was only consumed from lobby updates. Team-slot updates now also consume
the pending map transition and return immediately after completion because
loading can invalidate the old UI objects. Regression tests cover success
without a lobby tick and verification failure from this selector path.

The preceding Drive-Thru Danger race recorded 112 takeoffs, 43 without a
jump-speed reference, 20 above 10000 world units/s and 49 native recovery
entries. Maximum recorded takeoff speed was 13265.1. Itemless catch-up now
publishes zero during a referenced jump approach or airborne state, then
ramps back on the ground. This remains the authority's existing replicated
bonus decision. Recorded approaches and flights cap owner horizontal speed
at 105% of the reference; heading and vertical velocity remain native.
Without a reference the airborne floor is disabled, preserving native
deceleration instead of holding catch-up overspeed. Normal catch-up and
human physics are unchanged. Tests cover immediate ground/air overspeed
limits, absent-reference decay, authority bonus removal, and the existing
human/unresolved-handle/remote-owner exclusions. Actual multiplayer starts
and shortcut landings still require a new gameplay session.


## Spatial speed through the flight (22:51 Team session)

Host 20261104_225114.log and guest 20261104_225117.log both consumed the
pending transition and loaded the Team race. The host still logged 25
missed-landing recovery entries and 12 other stuck entries. AI 3's first
missed landing followed a takeoff reference of 2367.2, while a later route
segment reported 3945.1 and the kart was held near 2485.6. This exposed the
fixed takeoff ceiling as an obstacle to following an accelerating recording.

The reference module now exposes the horizontal speed of its currently
matched spatial segment, clearing it when alignment/validity is lost.
Itemless AI uses that speed on the guided approach and throughout flight:
the ceiling follows the segment, and the lower speed target can rise at
6000 world units/s². Losing alignment or colliding cancels assistance rather
than retaining an old takeoff speed. Steering, position and vertical velocity
are not overwritten. Existing native-owner and positive AI identity checks
remain mandatory. Extra hop/trick impulse multipliers and forced perfect
trick flags are removed on both peers; they stretched flight time relative
to the recording. Native impulses are now unchanged for all karts.

Catch-up retains its host-authoritative distance/difficulty calculation and
existing published jump/airborne suspension. It resumes by its existing
ramp after the protected section. No new locally decided physics multiplier
is introduced on the guest. Native owner velocity replication carries the
spatial assistance. Flight diagnostics now include current route speed.

Tests cover one airborne trajectory accelerating from 1000 to 2500 and
braking to 500, missing-route cancellation, native vertical impulses, human
and unresolved-handle exclusions, and remote-owner exclusion. Reference
fixtures also run over 240 track/difficulty combinations and 212933 sampled
poses across seven laps. These are code/data checks, not proof of zero falls
in a live game; actual landings still need gameplay validation.


## Approach support and curved targets (23:02 session)

Host 20261104_230230.log still recorded seven missed-landing and two other
stuck recovery entries in the first lap. AI 4 took off at 2257.8 against a
3342.7 reference after leaving the precision corridor (214 units lateral
error). Speed support previously shared the strict 200/120-unit technique
alignment gate, and runtime discarded the prepared takeoff speed in favor
of the current, slower segment.

Speed support now tolerates local, forward-aligned terrain projections
within 350 units while hop/slide inputs retain their precision gate.
Large errors, reverse motion and nonlocal recovery do not receive this
extra tolerance. Current flight speed remains available despite vertical
phase differences; grounded below-route poses still cannot request jump
approach acceleration. Prepared speed blends from the current segment to
the takeoff segment during the final half-second of recorded approach,
and runtime now honors it. Curved path lookahead smoothly shortens based
on tangent changes over the next 0.4 seconds to reduce corner cutting;
straight-line lookahead is unchanged.

Regression tests cover 214-unit lateral drift without a hop input or loss
of airborne assistance, rejection at 500 units, reverse rejection, early
acceleration before a faster takeoff segment and a right-angle lane target.
Existing AI-only/native-owner tests pass, as does the 240-track/difficulty,
212933-pose reference data check. Multiplayer gameplay validation remains
outstanding; these deterministic checks do not prove every shortcut lands.


## Team scoreboard DNF snapshot

Late finish callbacks previously upgraded live completion state and rewrote
row opacity/awards after the scoreboard opened. Team results now freeze
completion separately from points after native final-result handling. The
host publishes an immutable finish mask indexed by the validated result
order. The existing 48-byte peer snapshot carries a valid bit at byte 43
and the mask at byte 44, under the existing host/map/nonce/sequence checks.
The mask survives bonus expiry and resets with the race; duplicate packets
and late finishes cannot revise it. Guests buffer results until the host
mask is available and keep unresolved rows dim during that wait. Normal
mode's existing points for DNF remain distinct from visual finish status.

Both clients must use the updated build: an older host cannot supply the
new final-mask metadata. The two-process UDP integration test passes with
bonus, difficulty, recovery and final-mask checks. CPU-only prebuilt dated
2026-11-04 23:17:11 WIB includes both the AI approach changes and this fix.
Live game validation of the late-finish panel remains outstanding.


## Terrain-hop timing at the repeated toxic shortcut (23:21 session)

Host 20261104_232131.log shows all six bots entering recovery around the
same first-lap shortcut near (-3600,-400,-1960). Logged takeoff speeds were
already close to the route reference. The local projection treated a kart
below the recorded arc differently from one above it, and early-hop timing
measured full 3D distance to the ghost marker. Height/lateral differences
therefore delayed an input that should be tied to forward lane progress.

Local terrain matching now projects horizontal progress on both sides of
the recorded arc, retaining the existing below/above height rejection and
3D global reacquisition rules. Terrain-hop preparation/press uses remaining
planar arc length from that local projection to the marker. Precision
alignment, consumed-marker guards and flat drift-hop behavior remain.

A regression uses the actual t508_toxic developer marker 356, with an
80-unit lateral offset and +/-180-unit vertical offset. It verifies an early
single hop rather than a late/missing press. Restoring the prior projection
and 3D trigger-distance rules in a temporary test build fails this exact
assertion; current code passes it, the 240 track/difficulty reference check
and AI/player physics regressions. No new vertical-force or velocity changes
were introduced here. Actual shortcut completion still needs in-game testing.


## Airborne trajectory control and corner-aware catch-up (23:32 session)

The latest host log 20261104_233255.log still had all six bots enter recovery
near the repeated shortcut. Speed-only assistance and hop-trigger fixtures
had not demonstrated actual landing. The safe-route fallback discussed in
chat was rejected and was not implemented.

Reference guidance now exports a local projected point, full 3D velocity,
and upcoming tangent change. On the native-owning Itemless AI only, airborne
motion applies bounded horizontal AND vertical velocity corrections toward
that trajectory (12000 world units/s² cap, bounded positional correction).
This intentionally changes vertical velocity; the earlier statement that
all airborne assistance preserves gravity/vertical motion is no longer true.
It never writes position or orientation. Missing/nonlocal data, errors over
350 horizontally/500 vertically, nonfinite values, stopped karts and recovery
states disable it. All referenced tracks use the same controller. Precision
hop rules and native input remain; no map-specific flight exemption exists.

Ground corner speed is limited toward the reference speed. Host catch-up
publication also reduces bonus for sharp native route curvature in Normal
and Itemless, and uses reference curvature for Itemless. Thus U-turns do not
retain the straight-line bonus. Native peers consume the same host bonus;
only the owner writes motion and native replication carries it to followers.
Motion correction dt follows the kart's native simulation timer when present,
so repeated render calls in one simulation tick cannot accumulate assistance.

Tests now integrate motion under gravity through left/right 180-degree air
turns with different initial speeds, and through the actual toxic marker 356
flight with a weak takeoff. The real-data fixture reaches (-2553,793,-1948),
near the recorded landing (-2596,734,-1932), after 24 simulation steps.
These are simplified closed-loop physics tests, not full engine collision
simulation. Tests additionally prove native-owned AI motion changes, while
remote-owned AI, resolved/unresolved human handles and Normal player physics
remain excluded; zero-delta simulation calls add no correction. All 240
track/difficulty reference checks still pass. Real-game landings remain
unverified, so zero falls cannot be claimed.


## Reference seam continuity and ground pace (23:51 session)

My screenshot shows fastest AI 2:11.57 against the supplied dev
benchmark 1:50.85. Host 20261104_235117.log records AI 3 in recovery for
88.46 seconds, while AI 5/6 recover for approximately 6.5 seconds each.
AI 3's earlier matching reports include sample 2141 (recording end) followed
by sample 10, despite still being on race lap 1. The selected developer data
has 2143 samples spanning approximately 110.35 seconds.

The local projection window now continues across the finish seam to the
recorded forward loop_start, matching the steering target's existing wrap.
Previously projection could remain clamped at the final segment until the
kart moved more than the 1600-unit reacquisition radius away. Wrapping local
progress rearms the recording's hop markers; it does not force a map-specific
fallback or move the kart. Tests cover continuous spatial advancement across
a synthetic seam and the forward-aligned seams of all installed references.

Itemless ground pace now approaches the recorded segment speed gradually
when the kart is within 120 units horizontally/100 vertically and its heading
is within the .95 dot-product alignment threshold. It does not accelerate
reverse/off-line poses, alter vertical velocity on the ground, or cap straight
catch-up to baseline. Existing corner braking and owner-only airborne control
remain. Reference, landing/U-turn simulation and player/owner exclusion tests
pass. Actual game race times and recovery reduction are not yet verified.


## Catch-up demand retention and passing (00:02 session)

Host 20261105_000231.log has 172 periodic catch-up reports but only 12 with
positive applied bonus, maximum 0.51. The host was ready, so transport did
not explain the mostly zero values. Jump/corner restrictions overwrote the
same accumulator used to ramp demand, repeatedly restarting catch-up.

The host now retains ramped demand separately from the applied restriction.
It continues to calculate distance/speed need during protected sections and
publishes the allowed bonus, resuming accumulated demand when protection
ends. Ground takeoff protection and its speed ceiling now cover the final
0.35 estimated seconds to the marker, instead of the full 1.2-second
reference preparation horizon. Estimated arrival accounts for kart versus
recorded segment speed. Airborne protection and corner limits remain.
Logs expose demand, applied bonus, jump protection and native turn allowance.

Traffic avoidance selects the nearest racer ahead instead of summing
opposing requests that can cancel for a racer in the middle of a pack.
Grounded straight early approaches (>0.6 estimated seconds before a known
jump) may pass; airborne lanes, imminent takeoffs, unknown terrain and sharp
corners remain protected. Existing bounded lane offset is unchanged.

Regressions verify demand survives a jump and resumes without another ramp,
early approach can apply a bonus while imminent takeoff cannot, and a farther
opposite-ID racer cannot cancel passing the nearest racer. Flight/U-turn,
reference seam, all-track and human/owner exclusions pass. CPU-only prebuilt
includes these changes; stronger catch-up and reduced queuing remain to be
verified in-game, and matching the developer lap time is not guaranteed.

### 2026-11-05 — Extreme/Nightmare pad reserve maintenance

I expanded the requested Nightmare-only pad maintenance to Extreme and Nightmare.
Native TT ghosts contain movement and input snapshots, not a usable pad-colour/reserve timeline.
The implementation therefore retains the fastest *live native pad boost entry* before native boost ageing,
in Normal and Itemless, private and offline races. It maintains a two-second reserve floor plus the
current simulation step, rather than adding an infinite duration or modifying shared boost data.

Native evidence: CKartBoostData +0x3c is the boost-pad flag (property table includes `_isABoostPad`;
e2e8e7 tests it for pad-specific handling). Native e2e6b0 compares incoming boost speed at +0xc and
carries reserve to the faster boost; slower red does not replace a live blue. Hook e75250 (EU e75240)
runs before e75750 entry ageing/expiry. Cancelled/expired boosts are not resurrected, and existing
wall/brake/item cancellation remains native. Only the fastest active pad entry is maintained, allowing
old lower-speed entries to expire. No shared archetype data or player boost is changed.

The same rule runs on native boost entries on both peers, with the existing replicated AI difficulty
and positive live AI identity check. There is no new unsynchronized colour cache, owner-only reserve
mutation, or new network packet field. Original native boost acquisition, maximum-speed selection,
exhaust state, and ownership transfer continue to supply the actual state.

Validation: test_race_options passes repeated ageing over hundreds of frames, red -> blue -> red,
Normal/Itemless, offline/private, owner/follower timesteps, lower difficulty exclusion, unresolved human
handle exclusion, item/non-pad exclusion, cancellation and expiry, stale identity, battle/public gates,
paused/invalid timestep, replicated difficulty precedence, and unchanged shared data. Native instruction
contracts pass against both installed USA/EU 1.21 dumps. CPU-only build uses BB_SKIP_GPU=1.
Actual two-client gameplay with pad acquisition/ownership transfer still needs playtesting; unit tests
and native byte validation do not establish an absolute no-desync guarantee.

### 2026-11-05 — Keep acquired fire while braking; race other AIs

Latest host log `20261105_003508.log` and guest `20261105_003511.log` load the reserve patch.
Host trace confirms repeated reserve=2.000, but includes 19 sampled transitions from >=1.9 to zero.
Those logs alone do not identify cancellation causes or native visual tiers. Native analysis confirms
that e2f1d0 can remove an entry on braking regardless of the added carry reserve, and e92e90 can remove
it on wall contact. I explicitly requested that acquired fire not extinguish when braking.

The fastest live pad entry for a positively identified Extreme/Nightmare AI now bypasses the native
keep/stop cancellation decisions. The shared Itemless contact wrapper invokes a scoped pad-preserving
stop wrapper: only that entry is temporarily marked maintained during the native stop call, then its
contact flag is restored. Other boosts remain cancellable. No shared boost data, player flags, braking
input, or steering is modified. Native speed selection and exhaust presentation use the retained actual
pad entry, retaining blue over subsequent slower red. Map/racer resets still clear native state.
The earlier note that all native cancellation is retained is superseded by this explicit requirement.
Five-second diagnostics now report native pad speed, applied boost, reserve, and native exhaust tier.

I also requested that AIs race both humans and other AIs with different catch-up pacing. The host now
selects the nearest unfinished racer ahead by native race ordering, regardless of human/AI identity.
Each stable AI id gets a distinct smooth 8–15.7 second catch-up cycle, peak strength and acceleration
response; distance, difficulty, overtaking/removal, and corner/airborne restrictions still apply.
Only the host evaluates the cycle and publishes the quantized resulting bonus through the existing
per-AI snapshot, including when native ownership migrates to a guest. No per-guest random decisions.

Validation: race-options suite passes repeated braking/contact while maintaining pad reserves,
red->blue->red and human/difficulty/mode exclusions; Itemless contact regression passes. USA/EU native
hook contracts pass. AI catch-up transport/expiry tests pass; the two-process LAN test additionally
checks eight different per-AI values. CPU-only prebuilt rebuilt with BB_SKIP_GPU=1. Visual continuity
and race spacing still require actual host/guest gameplay; tests do not prove no desync in every case.

### 2026-11-05 — More than two clients / full eight-player room

Audit found two concrete transport gaps:
- AI readiness previously accepted any single fresh guest handshake. The host now counts remote
  human racers and requires that many fresh peer handshakes; a missing/stale guest disables bonuses
  until the native roster confirms departure. Tests cover every room size from two through eight.
- Direct LAN discovery previously required an AI update to set `require_peers`, and all control
  traffic depended on BB_ONLINE_PING. A full-human Team room could therefore fail to deliver its
  authoritative final mask, leaving guest results pending. An active race synchronization session
  now enables control discovery independently of AI presence and the optional ping display. Nickname
  initialization also runs when ping display is disabled; the disabled display itself stays hidden.

The LAN regression now forks independent client processes in 2-, 3-, 4-, and 8-client meshes, delays
one guest, exchanges differing per-slot bonuses, and runs four map resets including a repeated track.
It covers ping enabled/disabled and an eight-human/no-AI room where only final result metadata is
published. All four configurations passed. This verifies the real UDP/control path, not eight full
native game instances. Unit readiness tests separately prove that one of seven guests is insufficient
and that a stale required guest cannot silently reduce the required roster.

Lobby/capability regressions pass for rosters 1–8 across Itemless, Cup, Team, and modified difficulty.
Room-mode tests pass delayed publication, asynchronous verification, frozen Cup rules, leave/rejoin,
and stale responses. Team scoreboard regression now exercises early/late authoritative masks and
late-DNF callbacks from all eight local player viewpoints; USA/EU online-mode suite passes. Race AI
and authority/expiry tests pass. CPU-only prebuilt rebuilt; GPU skipped.

Still requires real gameplay with 3–8 clients, including adverse network conditions and host departure.
No claim that automated tests prove the absence of all desync or native-engine errors.

### Normal catch-up balance and automatic release policy (2026-11-05)

- Normal AI catch-up is capped by difficulty (4/6/8/11/15/20 percent), scales by time gap, and ends near the target or after passing it. AI target speed excludes its own catch-up, preventing recursive amplification. Itemless tuning is unchanged. Per-AI pace and host replication remain active.
- Client embeds a common platform-independent release identity in registration and private preflight. Clean tags / official BB_VERSION packaging identify releases; modified local sources use a dev fingerprint. A release-specific acknowledgement prevents old backends authorizing the new private protocol.
- Backend source in sibling RyuNET-Demonware-CTR now fetches the GitHub latest stable SacredFire release on startup and every five minutes in the background (10-second timeout). Game HTTP handlers read only an in-memory snapshot. Last verified policy survives transient failures for at most 24 hours; no verified policy fails closed for extended private modes. No manual CTR_PRIVATE_RELEASE setting remains.
- All private extended participants must match that release and relevant feature capabilities. Public Itemless remains capability/protocol-based across releases. Background refresh does not disconnect a running race.
- Validation: AI race options tests, private/public client gate tests, Python version identity tests passed. Full backend tests with Go race detector passed, including eight-member release checks, public exception, refresh failure/expiry and nonblocking gate reads during a stalled fetch.
- CPU-only prebuilt completed with BB_SKIP_GPU=1; out/VERSION is dev-bc6e2c9f79ee1083a9d6. This development identity intentionally does not impersonate a published release. Backend files are updated locally, not deployed/restarted; coordinated official client/backend rollout is still required. Actual multiplayer gameplay has not been re-tested for these latest changes.

### PR validation on current upstream main (2026-11-05)

I rechecked the changes after rebasing onto game main 035f0d7 and server main 1ebe670.
The CPU-only build succeeded (GPU skipped), producing dev-9d5c0f73e0296405cbec.
I tested the online-mode, race-options, AI catch-up, TT-reference, Itemless-contact,
public/private compatibility, room-mode and ping regressions successfully. I also tested
real UDP meshes with 2, 3, 4 and 8 independent client processes, including four map resets,
ping disabled and a full-human room. These are transport tests, not eight live game instances.
I isolated native braking from timed route corrections in its test fixture so that the
braking assertion no longer depends on wall-clock scheduling; route guidance has separate tests.
I verified native hook contracts against both USA and EU dumps (three tests per edition),
release identity tests and shell syntax. The full Python run completed 448 tests with 68
skipped and the same nine test_probe failures recorded in out/pr-probe-baseline.log.
I tested the committed backend from a clean temporary checkout with go test -race ./server;
it passed. Running it directly in the local server checkout first hit an achievements fixture
panic, so the clean checkout isolates the result from local server state.
The full build.sh --test attempt stopped at the timing-dependent fixture before its correction;
the corrected and remaining relevant component suites were subsequently run separately and passed.
I have not revalidated final AI visuals/balance or 3–8 full native clients in live gameplay,
and have not built Windows or deployed the backend during this PR preparation.
