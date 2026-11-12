# TECHNICAL SPECIFICATION: CTR NITRO-FUELED NATIVE RANKED SYSTEM (PC PORT / SacredFire)

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
* **The launcher (Python)** is not in the results path. It only reads: the Online tab shows the
  nickname's two ratings from `GET /ryunet/ranked-profile` (`ryunet_http.ranked_profile`).

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

### Online session isolation

Confirming Ranked, Matchmaking, or Private from Online main starts fresh transient
state before the native menu route runs. Previous public confirmation/nonces,
ranked results, party preflight, Team/Cup selection and scoreboards, pending map
loads, and native Cup track handles are retired. Persistent player MMR and the
connection's compatibility registration are retained.

A guest joining a different party queues the same reset before applying its host's
native settings. Worker callbacks only queue GUI cleanup; native handle release
runs on the game thread, before settings receipt, selector input, map loading, or
the next frame. HTTP mode verification and polling keep generation checks, so
responses from a departed host cannot alter the new session. Repeated information
about the current party does not reset a live Cup between its tracks.

An unresolved host Cup is rejected instead of being silently converted to Single.
A subsequent private map load is refused until valid host settings replace the
rejected payload. Native mode/track replication and the final Itemless verification
remain authoritative on both host and guest.

### Skill matching

Matchmaking uses the selected mode's authoritative MMR, including provisional ratings during
placements. Players without a profile start at 1000. Tiers and leaderboard ranks do not
partition queues. Each party uses its highest member's server-owned MMR as its matchmaking anchor, never an average. Any rank spread is allowed, including placed/provisional parties; provisional internal MMR participates in matching but remains hidden. A highest/lowest-tier party faces the highest member's peers. A placed/provisional party uses whichever internal rating is higher (new profiles start at 1000).

| Both sides have waited | Maximum MMR spread |
|---|---:|
| 0–19 seconds | 400 |
| 20–44 seconds | 700 |
| 45–89 seconds | 1000 |
| 90 seconds or more | No hard cap; closest available MMR remains preferred |

Every pair of searches in a new room must fit the range. Existing-room joins check every
player and use both the incoming search's wait and the room's time since its last join.
Room merges use both rooms' idle time. A long wait on one side cannot widen the other
side's range. After both have waited 90 seconds, removing the hard cap prevents permanent isolation in a small Ranked population. There is no requirement for 200 players to be online; a room still needs two human seats.

Geographic proximity remains first priority. Within the same distance band, closer MMR
comes before room fullness. Rooms still need only two players, not eight. Searches retry
every two seconds, including when geographic filtering is disabled. Existing-room ratings
refresh when checking candidates after at least 15 seconds since the last rating capture.

### Season admission gate

`runtime_ranked.c` requests `GET /ryunet/ranked-status` on its existing background worker.
The response contains `season`, `enabled`, and `retry_after` in seconds. Missing, malformed,
or unsuccessful responses fail closed. The Ranked menu stays accessible in both modes.
While admission is closed or being checked, the native Go to Lobby button is dimmed and its
Confirm event is intercepted before the native graph can transition to the lobby.

Confirm starts a fresh status check when admission was open. The existing deferred-input
mechanism retains native handles, not raw UI pointers, and replays Confirm only after an
open response. Back, focus loss, a changed session, or timeout cancels that pending entry.
A closed response waits the server's `retry_after` before another request; transport failures
retry after five seconds. A zero retry interval while closed is limited to one second.
The search hook also checks admission. The server remains authoritative if a reset starts
between the preflight and the search.

A changed `season` invalidates both mode profiles and highest-tier badges while preserving
the registered player names for immediate refetch. Status handling does not leave an active
room or stop a race. Race reports continue using their captured room and mode; the server
assigns those results to the draining season.

Tests cover response validation, retry timing, season invalidation, both modes, deferred
Confirm, and Back cancellation. Native dimming and transition behavior still need live QA.

### Hall of Fame

Ranked and Hall of Fame clone the native Online Matchmaking button with independent
state. Both preserve the original destination project handle in VSC +0x38. This field
is not a self-button reference: low RHND values index EXID hashes, while high-bit values
index the handle subset of EXNM.
HOF Confirm arms a one-shot request. The native asset-opening entry at 0x986170 consumes
that request and forwards the same manager, options, player, context, and delay to the
named opening entry at 0x986820 with `Octane_HallOfFame`. It temporarily enables the
native streaming option at options +0x21, then restores its previous value. Native
0x987450 only invokes the package loader when this option equals 1. The request is
consumed before the named path calls the asset path again. Other opens preserve the
native asset path.
The native button graph is unchanged; no custom script class or directional input replay
is introduced. HOF's Back target redirects to Online Main and restores focus.

The native Matchmaking graph writes 0 to `MainMenu_Base_ButtonIndex`; Private writes 1.
Lobby Main's native switch uses this value to call `setFocus_VSC`, then resets the saved
index. Ranked and HOF preserve the clone's index 0 and update the root's native case-0
and default destination handles to the selected button. Runtime VSC instances do not
retain the serialized IGZ field layout: live instances had no dynamic-memory pointer
at +0x18. The native reflection lookup at 0x14a7250 resolves the fields
`Gui_Placeable_id_rcxu6uoi` and `Gui_Placeable_id_bdnprt14`; each returned meta-field
provides its runtime offset at +0x14. `Gui_Placeable_id_gto91nsu` retains Matchmaking's
destination. Selection stores only a button identity across menus. Targets are resolved
from the current tree, including composition content, and native handles manage their
lifetime. Private and other switch cases remain unchanged. No synthetic navigation
input is sent.

`scripts/hall_of_fame.py` retains the native project graph, navigation, selector visuals,
fonts, shadows, and row composition. It removes the time-trial data behaviors and Track,
and creates eight independent native row compositions for the displayed window. The
native footer displays the local player's position in the selected population when available. The existing
HOF is excluded from Lobby Main's TDEP and the Online domain manifest. Its complete
standalone streaming package and loose-file dependencies remain available for the named
opening path. This isolates HOF project initialization from entering Online.
Each row composition is a named export referenced through a native RHND handle.
Runtime traversal resolves the placeable's +0x38 asset handle before reading composition
content; treating that handle as an asset pointer skipped the row and selector trees.

The retained Leaderboards root graph starts its entry animation on
`CGuiEventLeaderboardReady`. Removing the native time-trial list behavior also removes
the original event producer. After initializing HOF's root, the client therefore creates
this event with native factory 0x1104d80, dispatches it through the project's native
virtual event handler at +0xb0, and releases it with 0x149d260. The event is allocated by the engine, never fabricated on the stack. HOF owns
its loading and empty labels, so the event selects the native HasData presentation.
It is sent once per HOF opening, not on filter changes. The callback follows its +0x20
project reference, that project's root wrapper, and the wrapper's composition content
to find the tagged HOF root. It passes the project separately for event dispatch,
matching the native leaderboard producer at 0x9aff57.

Live debugger tracing corrected the earlier interpretation of the open handle: it was
a streaming placeholder, not a loaded HOF view. At 0x985180 the pending slot was in
state 3, but 0x14fad90 returned no directory for `UI/Octane_HallOfFame.igz`. The asset
passed to 0x986170 was null, and the slot was then retired. The active file map contained
the HOF archive and UI files but omitted `packages/generated/ui/octane_halloffame_pkg.igz`.
Preparation now publishes that manifest in the loose file map as well as the archive;
the regression checks both its bytes and mapped path. HOF opening and Ranked Back focus
are verified in game.
Rows and tier icons must also join the native next/previous traversal chain; child-table
registration alone is insufficient. Tier icons share the tier label’s parent so they inherit
the same row transforms. The HOF root retains ReturnToMainMenuOnDisconnect for its update hook.
The renamed project also packages native Leaderboards language tables under
`octane_halloffame_<language>.lng`, matching the UI loader’s project-based lookup.

`runtime_hall_of_fame.c` fetches
`GET /ryunet/ranked-leaderboard?mode=0|1&count=100&season=<id>&name=<nickname>&population=everyone|friends`
using the existing proof-signing HTTP transport. Everyone is public. Friends requires a
signed request and includes the authenticated account plus accepted native friends, including
offline friends. Pending friend requests are excluded. Filtering precedes the 100-entry limit,
so friends beyond the global top 100 remain eligible. Friends responses are not cached.
The client sends one request for either population and does not supply arbitrary friend names.

HOF rows sort by `rank` in ascending order. Everyone uses the global rank. Friends uses
consecutive population-relative ranks; `global_rank` separately preserves the full-ladder position.
At the first visible row, Up passes back to native navigation to reach the selectors.
The native time column displays the API's `tier` name and the matching Lobby Members
sprite for the selected mode. It does not derive tier from MMR or display race times.
A separate fitted column displays MMR for leaderboard rows and the placed local player.
Placement MMR remains hidden.

The server supplies `players`, optional `player`, `current_season`, and `seasons` containing
`id`, `started`, `ended`, and `active`. The API returns seasons newest first; the client
sorts them oldest to newest for navigation. Entry defaults to
the current season at the right end; left/right navigation clamps without wrapping. A mode,
population, or season change clears stale rows and starts a new request. Late replies from
an earlier selection or closed menu cannot replace the current view. Native list direction
input scrolls the displayed window. Failed requests show an unavailable state; Confirm retries.

The optional `player` is independent of the page limit: a ranked player outside `players`
still has a position; an unranked player returns `player:null`. Without `name`, `player` is
omitted. `player.in_leaderboard` uses the selected population's `rank < count`, strictly:
with `count=100`, rank 99 is true and rank 100 is false, although row 100 remains on the
page. This flag does not remove rows or change numeric ranks. Valid counts are 1–500;
omitted or invalid counts default to 100. The same contract applies to archived seasons.
An unknown archive returns HTTP 404. `mode=all` returns `normal` and `itemless` boards.

The website Hall of Fame has a separate public API with 50-entry pages selected by `offset`,
using `rows` and optional session-account `mine` instead of the game's `players` and `player`.
Its pagination does not use the game's `count` or `in_leaderboard` contract.

A changed `ranked-status.season` refreshes the board. If the user was viewing the current
season, selection follows the new current season; an explicitly selected archive stays selected.
HTTP workers own copied query data and never retain guest UI pointers. Native Back/unload
invalidates pending work. The native return route to Highscore Select is redirected to Online
for the Hall of Fame flow.

C tests cover parsing, season ordering and bounds, modes, server-side Friends responses,
request count, stale replies, retry, and scrolling bounds. Python tests check independent row
objects, the native opener, streaming dependencies, and the existing widescreen preparation.
Hall of Fame rendering, focus, scrolling, refresh, and Back transitions are verified in game.

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

5. The worker retries a `pending` answer a few times over ~16 s, then gives up (the rating was
   recorded server-side regardless). The last scored delta joins the lobby card:
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

* **Lobby card** — the queue label plus the last scored delta in this room.
* **Lobby Members** — each placed player's tier uses one of seven stock kart-decal sprites,
  fetched in one batch from `GET /ryunet/ranked-tiers?names=...&mode=0|1`. Unplaced players have
  no icon. The JSON response contains `tiers` and `mmr` maps keyed by lowercase nickname. The icon
  follows the nickname in the member row.
* **Launcher Online tab** — the Ranked row reads `GET /ryunet/ranked-profile?name=<nick>&mode=0|1`
  (or `mode=all` for both modes). Placed profiles include `mmr`, `tier`, and `rank`.
  Unplaced profiles expose only placement progress and race count; no provisional rating
  or tier is published.
* **Roadmap** — per-row tier marks on the results screen, opponent tiers in the lobby,
  placement-progress banner.

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
