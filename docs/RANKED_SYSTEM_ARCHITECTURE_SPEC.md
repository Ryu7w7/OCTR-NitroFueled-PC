# TECHNICAL SPECIFICATION: CTR NITRO-FUELED NATIVE RANKED SYSTEM (PC PORT / SacredFire)

## Implementation status

Reviewed on 2026-11-13 (Asia/Jakarta). The [Ranked roadmap](RANKED_ROADMAP.md) separates
implemented features, outstanding live QA, and unimplemented work. Implemented does not
imply production deployment. The current branch includes device-based admission and elapsed
matchmaking status; latest UI/device changes still need live multi-client validation.

## 1. SYSTEM OVERVIEW

This document is the client-side specification of the Ranked system. It replaces the earlier
draft that described a Python/ctypes bridge, a WebSocket API and PostgreSQL: none of that exists
in this codebase, and none of it is needed. Ranked reuses the transport every other online
feature already uses.

### How the pieces actually talk

* **The native mod (C, `src/`)** owns race telemetry and the ranked HTTP traffic. At the
  validated host results event it reads the finish order and posts it, signed, straight to the
  RyuNET server (`runtime_ryunet_transfer`, the same path as chat, spectate and name colors).
* **The RyuNET service** validates race reports and provides authoritative ratings,
  seasons, match history, and archived ladders.
* **The launchers (Linux and Windows)** are not in the results path. Ranked profiles and
  MMR information are displayed only in the native game UI.

There is no launcher-to-game IPC. The launcher prepares files and environment variables before
boot; the mod talks to the server itself, from a worker thread, exactly like spectate does.

## 2. CORE RULES

1. **Exactly two ratings**, chosen by the queue's mode, never by party size:
   * **NORMAL** — item-enabled races.
   * **ITEMLESS** — pure racing.
2. **Both start at 1000** with **5 placement matches** each. Placements use K = 180;
   subsequent races use K = 90. A profile opens with its first ranked race. Until a mode
   completes placements, its profile reports `ranked:false`, remaining `placements`, and
   `races`, without `mmr`, `tier`, or `rank`. An untouched mode has five placements and zero races.
3. **Solo FFA is the only queue in v1.** The matchmaking server forces the Race playlist and
   builds 2–8 player rooms; team formats (4v4, 2v2v2v2) need a team queue that does not exist
   yet (roadmap).
4. **Seasons.** Seasons last 30 days by default. Automatic or manual closure starts a
   15-minute drain: new Ranked searches stop, while confirmed rooms can still submit
   results to the closing season. After the drain, old-room reports are rejected.
   The final placed ladder is archived and
   every rating **soft-resets halfway back to 1000** — `new = 1000 + (mmr − 1000) × 0.5` — with
   the 5 placement matches restored. Career totals (races, wins) are never reset.

## 3. MATCHMAKING (already shipped)

The queue separation is server-owned: the client injects `{"octr_ranked":{"version":1,
"mode":0|1,"nonce":"…"}}` into the game's own matchmaking search (`runtime_public_itemless.c`),
the server reserves the `octr_ranked_version` filter key so ranked searches can only ever match
ranked rooms, and injects an `octr_ranked_v1` confirmation (`1/<room>/<mode>/<nonces>`) into the
backend lobby document. A room is only joined when that confirmation matches the client's own
nonce. Parties preflight through the signed `GET /ryunet/ranked-party` first.

### Device admission and search timer

Ranked allows parties of at most four, and at most four active Ranked accounts per device
across solo searches, parties, and rooms. Every member registers its own SHA-256 hash of
Linux machine ID or Windows MachineGuid with a SacredFire-specific prefix. Raw OS IDs are
not transmitted. The server reserves slots atomically and releases matching reservations
on cancel, room leave, or disconnect. IP addresses do not affect this rule, and local testing
uses the same four-account limit. Older clients need an update for Ranked.

Members still race as solo opponents and receive individual MMR changes. Ranked must find
a non-overlapping outside search. Public solo players and parties of two through seven
also need an outside search; a full eight-member Public party can form a room immediately.
Private can play without outside opponents. Room capacity remains eight. The Ranked party
limit applies across devices, independently of the per-device limit; Public supports eight
without the Ranked device cap. The client blocks Ranked entry for five or more members
with a native host notice. If a fifth member joins during search or a confirmed Ranked
lobby, it cancels Ranked through the native menu path, preserves the party, and notifies
the host. Search cancellation and confirmed-room leave use separate native input routes.
Only members in a validated server search receive Ranked confirmation.

Public Matchmaking and Ranked show elapsed search time up to 03:00, then cancel through
the native matchmaking path. A background worker polls signed `/ryunet/matchmaking-status`
once per second while the Public lobby is visible, so followers share the leader's elapsed
time without HTTP on the game thread. Timeout dispatches a private native GUI input signal
to a dedicated search-cancel graph. It skips `quitParty` and lobby disband/reload, preserves
the Ranked/Public context and selected mode, and uses the native Back animation/destination.
Party guests retain the native party waiting route and registration needed to follow a new search.
The generic rejection path remains separate. Ranked shows the server's available range: 400 MMR,
700 after 20 seconds, 1000 after 45 seconds, and no hard cap after 90 seconds. Both sides
still need the threshold to match. Stale range data displays a checking message. Public
shows elapsed time only. A matched room hides this panel and uses the native countdown.

The passive panel reuses the native countdown artwork and anchor without copying its
behavior graph. Text uses the artwork's anchors, the original native text rotation, larger closely spaced
centered fitted boxes, and a
complete native traversal chain. These layout/traversal corrections follow a live report
of overflow and flicker; their live retest remains pending. Its generated archive patch uses the existing preparation cache, including
source invalidation and stale-file removal; warm boots reuse prepared output.

The OS identifier is client-reported, not hardware attestation. Modified clients can spoof
it, so this limit reduces ordinary multi-client boosting without proving distinct humans.

### Party-preserving menu navigation

Pressing Back in Private or Public/Ranked only opens the native confirmation dialog.
Cancel does not change party, search, or elapsed time. The accepted exit's destination focus
commits queue/race cleanup; timeout explicitly invokes the same cleanup. It uses native session leave with party
retention, direct race-session reset, and party-lobby close without restarting search.
The host then queues the native Quit with Party notification for followers. Timeout uses
the same cancellation; followers wait for the host instead of leaving independently.
The inbound party-return handler distinguishes menus from a frozen race. Menu guests use
a dedicated host-closed input, using the native host/guest branch to open Waiting for Host
with Customization only, or explicitly load Online Lobby
when no matchmaking project is active. Native race-exit handling is retained during a
frozen race; it must not be used to send menu guests to Start Screen. The domain archive
uses the modern update2 matchmaking graph/UI pair because its original graph lacks this route.
The guest branch registers `Octane_Lobby_WaitingRoom` with native `setLobbyMenuName_VSC`
before opening it. Otherwise the next `CGuiEventEnterLobby` targets the closed matchmaking
project and the guest remains waiting. Ranked/Public/Ranked re-entry is live-confirmed.
Private focus rearms the lifecycle and retains its GUI handle for the native
`CGuiEventPrivateLobbyClose` route. This avoids treating an active Private session as an
already-returned guest and avoids the scene reload fallback. Repeated Private entry/exit
is live-confirmed. The Private close route also updates the registered menu name at
race manager `+0xdf0` to Waiting Room before dispatch, matching native enter-lobby lookup.
Private-to-Public/Ranked re-entry is also live-confirmed in the two-client setup.

Lobby graphs replace `CNetworkNodes::stopMatchMaking_VSC` and Private's `disconnectAll_VSC`
with `resetNetworkSessionManager_VSC`; the Private reset alias is normalized too. The named
matchmaking stop calls the asynchronous disconnect path, whose callback at `0xa181e0`
resets the party. Live diagnostics captured its reset call at `0xa181fd` with a connected
guest, followed by zero connections and no inbound return notification at the guest.
Unrelated native sync-stop functions and their call sites are left unchanged.

Menu lobby-closed status and native UI cleanup remain. Main's explicit offline-exit graph,
explicit Leave Party, and network kick/disconnect retain their native behavior. PSN
race-session cleanup preserves host-follow state and session records while native
`hasActiveParty_VSC` transport count remains positive; race roster size is not sufficient.
Old status replies cannot revive cancelled searches, and polling stays suspended until
lobby re-entry. Both update2 and Online-domain graph copies use the existing preparation
cache with source invalidation. Repeated menu transitions are live-confirmed in the
two-client setup; 180-second timeout party retention and explicit Leave Party/offline
cleanup still require dedicated live checks.

## 4. RACE-END REPORTING (the mod's job)

The host's game builds the finish order and broadcasts it; **every client's mod holds the same
host-validated permutation** (`runtime_online_mode.c capture_finish_order` plus the finished
mask from `team_racer_finished` / the host's AI-catchup mask for guests). At that event, when
`runtime_ranked_active()`:

1. `runtime_online_mode_ranked_order` flattens the order into `{name, finished}` pairs — the
   in-game name of every human racer (AI fill has no name to report and is skipped).
2. `runtime_ranked_race_end` hands them to the worker thread (`runtime_ranked.c`, started from
   `runtime_net.c`), which posts:

   ```
   POST /ryunet/ranked-result?name=<me>          (X-RyuNET-Proof signed)
   {"room":42,"mode":0,"track":"electron_avenue",
    "order":[{"name":"A","pos":1,"dnf":false},{"name":"B","pos":4,"dnf":true},
             {"name":"C","pos":3,"dnf":false}]}
   ```

   `pos` is the on-screen finish position: AI fill keeps its slots, so a human who
   finished 8th behind six AI is recorded and scored 8th — honestly.

3. The server scores the race when **the room's own host reports** — the same authority the
   game itself uses. Guest reports wait (pending) and corroborate or contradict the host's
   order; a race whose host never reports is void. A race is identified by
   (room, mode, the set of finish-order PIDs), so a guest that posted moments before the host
   learns its own delta by retrying.
4. The response is an authoritative snapshot of this race, including `mode`, `race`,
   `season`, the reporter's outcome, and a `participants` array. A placed reporter receives:

   ```json
   {"status":"resolved","race":7,"season":1,"mode":0,"delta":45,"delta_applied":45,
    "ranked_before":true,"mmr_before":1000,"tier_before":"Gold Mask",
    "ranked":true,"mmr":1045,"tier":"Gold Mask","placements":0,
    "participants":[{"name":"A","pos":1,"dnf":false,"dc":false,"delta":45,
                     "ranked":true,"placements":0,"mmr":1045,"tier":"Gold Mask"},
                    {"name":"B","pos":2,"dnf":false,"dc":false,"delta":-35,
                     "ranked":true,"placements":0,"mmr":965,"tier":"Silver Nitro"}]}
   ```

   Before placements finish, `ranked` is false and `mmr`/`tier` are omitted; this is still
   a valid resolved result. `mmr_before`/`tier_before` appear only when `ranked_before`
   is true. Each participant includes `name`, `pos`, `dnf`, `dc`, `delta`, `ranked`, and
   remaining `placements`, with `mmr`/`tier` only for placed participants. `delta` is the
   scoring delta; `delta_applied` is the actual rating change after the zero floor.
   Accepted retries return the same race snapshot, even after later races. Reports from
   an expired season are rejected rather than rewritten with the new season's profile.
   Clients must not require MMR during placements or assume a fixed small response size.

5. The worker retries a `pending` answer a few times over ~16 s, then stops polling.
   A pending guest report does not guarantee scoring: the host report must still resolve the race. The last scored delta joins the lobby card:
   `RANKED - NORMAL  +45 (1045 Gold Mask)`.

Disconnects are not reported by the client: the server derives them (a confirmed room member
missing from the finish order, or one whose connection is gone at scoring time).

## 5. SCORING RULES

Solo FFA compares each finisher with every other confirmed human racer:

* `expected = 1 / (1 + 10^((opponent_MMR - player_MMR) / 800))`.
* `actual` is 1 for finishing ahead of that opponent, otherwise 0. A finisher always beats
  a DNF or disconnected opponent, regardless of the opponent's reported position.
* The skill delta is `K × mean(actual - expected)`, with K = 180 during placements and
  K = 90 afterward. Winning against weaker opponents pays less; losing to stronger opponents
  costs less.

AI retains its on-screen finish slots. Add `BasePoints(screen_position) - BasePoints(human_position)`
to the skill delta, where human position counts reported humans ahead of the racer:

| Pos | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|-----|---|---|---|---|---|---|---|---|
| Base | +45 | +30 | +15 | +5 | −5 | −15 | −25 | −35 |

Round half away from zero, then clamp a finisher's delta to **−35 through +45**. Two equally
rated humans finishing P1/P2 receive +45/−35. The asymmetric caps allow mild inflation.

* **DNF** (still connected): flat −45 Normal / −55 Itemless.
* **Disconnect**: flat −40 Normal / −60 Itemless. This includes a confirmed member missing
  from the report or a DNF racer no longer connected at scoring time.
* Finishing always loses less than DNF or disconnect. Ratings never fall below 0.
* Normal and Itemless ratings, placements, and records remain independent. Team formats
  and their penalties wait for team queues.
* The **USF full-lap bonus** (+5 Itemless) remains a roadmap item and is not awarded.

## 6. TIERS

| Tier | NORMAL | ITEMLESS | MMR |
|------|--------|----------|-----|
| 1 | Bronze Wumpa | Sapphire Relic | 0–499 |
| 2 | Silver Nitro | Gold Relic | 500–999 |
| 3 | Gold Mask | Platinum Relic | 1000–1499 |
| 4 | Platinum Trophy | Sacred Fire | 1500–1999 |
| 5 | Aku Master | Ultimate Sacred Fire | 2000–2499 |
| 6 | Uka Legend | Time Crusher | 2500–2999 |
| 7 | Chaos Grand Prix | Oxide Overlord | 3000+ **and** the mode's top 100 |

A 3000+ rating outside the top 100 shows the tier 6 name. Lobby Members uses seven distinct stock
kart-decal sprites for the seven tiers; the server returns tier names, and the client maps them to
those sprites.

## 7. WHAT THE CLIENT SHOWS

- **Lobby:** native rank card, last resolved room delta, and tier badges fitted into Lobby
  Members rows. The mode context selects the rating; returning to Online clears stale mode state.
- **Ranked results:** local MMR change, old/new tier, placement progress, native animated badge
  and effects, and sounds for rating/tier changes. Placement results need no public MMR.
  Automatic Wumpa navigation is suppressed in Ranked; manual Next/Back remains available.
- **Online result rows:** mode-specific tier badges before human nicknames in supported
  finish/end panels. AI, Battle, and offline rows remain native; podium badges are excluded.
  This does not add an MMR delta column for every opponent.
- **Hall of Fame:** mode, Everyone/Friends, and season selection; tier names and badges;
  fitted MMR columns for leaderboard entries and the local player; native scrolling and Back.
- **Matchmaking:** elapsed search time up to 03:00, then native cancellation. Ranked shows
  server-owned range expansion; Public shows elapsed time only. Confirmed rooms use the
  original pre-race countdown.
- **Launchers:** Linux and Windows omit Ranked/MMR information. Placement progress,
  ratings, and tiers remain available in the native game UI.

### Background artwork and preparation

`scripts/assets/ranked_background.png` and `scripts/assets/hof_background.png` use the
native Matchmaking source orientation. The texture compiler flips PNGs vertically before
BC3 encoding; do not apply an additional flip or rotation when editing the reference.
The native pattern quad has no rotation or negative scale; Matchmaking and Private use
identical animation curves. Custom materials retain the native animation.
The extracted authoring reference is generated locally at
`out/research/native-online-pattern/matchmaking-png-source.png` (not a tracked asset).
Both updated PNGs are 256×256, non-interlaced 8-bit RGBA. PNG edits invalidate the existing
archive preparation cache; subsequent warm preparation reuses the output.
USA/EU texture compilation passes. In-game background pattern correctness was confirmed
on 2026-11-13.

Recent UI fixes have automated coverage; outstanding live checks are listed in the roadmap.

## 8. TESTS

* `tests/test_ranked.c` (build.sh --test `ranked-test`): the report the worker builds, the
  answer parsing, pending→resolved retries, the retry budget, room-change card clearing.
* `tests/test_online_mode.c` / `test_public_itemless.c`: the finish-order accessor and the
  queue/confirmation behaviour that ranked rides on.


### Online result badges and HOF rating columns

HOF displays a separate fitted MMR column for every listed entry and the local player's
row, next to the tier column. Placement MMR remains private under the existing server
contract. Tier names use a 180-pixel column; MMR uses a separate 120-pixel column,
with native fit-to-box text and the original row navigation intact.

Online race, CUP and team-race result rows show the selected mode's tier badge before
the nickname. Podium names remain entirely native and do not display tier badges.
Normal and Itemless ratings are never mixed. Players in
placements show Unranked; pending profiles show no fabricated badge. AI racers,
Battle and offline panels retain their native layout. Shared BattleTeam assets are
allowed for team races only; actual Battle gameplay is excluded at runtime.

Badge siblings use local native sprites and existing rank materials. Native nickname
keys, behavior pointers, text alignment and animations remain intact. Badge reservation
follows the native text rotation in both axes; badge placement respects vertical alignment.
The runtime reserves
space within the original nickname rectangle, fits text into the remaining width and
restores the original rectangle when badges are inactive. Lobby Members' UI and the
Single Race texture are not changed by this feature. All generated UI/archive changes
participate in the existing Online preparation cache and stale-output cleanup.

Imported native badge animations keep `RNEX` field references separate from `EXID`
material references. Both shrink and grow tracks resolve `igGuiPlaceable::_scale`;
an invalid field reference can crash native track creation when the result card opens.
