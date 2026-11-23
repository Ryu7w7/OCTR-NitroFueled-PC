# TECHNICAL SPECIFICATION: CTR NITRO-FUELED NATIVE RANKED SYSTEM (PC PORT / SacredFire)

## Implementation status

The [roadmap](RANKED_ROADMAP.md) separates implemented behavior from future features.
See [migration status](HOST_MIGRATION_PROGRESS.md) for transport and identity handling.
The client displays ratings returned by RyuNET. Server policy and deployment belong
to the RyuNET repository’s `docs/RANKED_SYSTEM_ARCHITECTURE_BACKEND_SPEC.md`.

## 1. SYSTEM OVERVIEW

This document is the client-side specification of the Ranked system. It replaces the earlier
draft that described a Python/ctypes bridge, a WebSocket API and PostgreSQL: none of that exists
in this codebase, and none of it is needed. Ranked reuses the transport every other online
feature already uses.

### How the pieces actually talk

* **The native mod (C, `src/`)** owns race telemetry and the ranked HTTP traffic. At the
  results event it captures race identity and finish evidence and posts them, signed, to the
  RyuNET server (`runtime_ryunet_transfer`, the same path as chat, spectate and name colors).
* **The RyuNET service** validates race reports and provides authoritative ratings,
  seasons, match history, and archived ladders.
* **The launchers (Linux and Windows)** are not in the results path. Ranked profiles and
  MMR information are displayed only in the native game UI.

There is no launcher-to-game IPC. The launcher prepares files and environment variables before
boot; the mod talks to the server itself, from a worker thread, exactly like spectate does.

## 2. CLIENT MODE AND PROFILE STATE

The selected queue chooses Normal or Itemless profile data. Never mix ratings,
tiers, placement progress or result generations between modes. An unfinished
placement profile can omit `mmr`, `tier` and `rank`; the client still displays
`ranked:false`, remaining `placements` and `races` correctly.

Parties race as individuals. The client reads admission and countdown readiness
from RyuNET rather than hardcoding start minimums or calculating MMR. Season-status
responses gate new Ranked entry; an already-running race retains its result flow.

## 3. MATCHMAKING (already shipped)

The queue separation is server-owned: the client injects `{"octr_ranked":{"version":2,
"mode":0|1,"nonce":"…"}}` into the game's own matchmaking search (`runtime_public_itemless.c`),
the server reserves the `octr_ranked_version` filter key so ranked searches can only ever match
ranked rooms, and injects an `octr_ranked_v1` confirmation (`2/<room>/<mode>/<nonces>`) into the
backend lobby document. A room is only joined when that confirmation matches the client's own
nonce. Parties preflight through the signed `GET /ryunet/ranked-party` first.

Both Ranked modes use exactly five laps on every track and never fill empty seats with AI.
Ranked Itemless disables racer-to-racer collisions and aligns starting positions. Ranked
Normal keeps native racer collisions and the native grid. Itemless also requires collision
rules capability 1 and confirmation version 2. Older Ranked contracts are rejected.
These fixed race rules do not change private, unranked public, or offline races.

### Admission and search timer

The client registers its own SHA-256 hash of Linux machine ID or Windows MachineGuid
with a SacredFire-specific prefix; raw OS identifiers are not sent. It advertises
its release and Ranked capabilities on the current connection and uses signed
party preflight. Rejected admission displays the server's response.

The client blocks Ranked entry for parties above four with a native host notice.
If a fifth member joins during search or a confirmed lobby, it cancels Ranked through
the appropriate native navigation route and preserves the party. Search cancellation
and confirmed-room exit use separate native inputs.

A background worker polls signed `/ryunet/matchmaking-status` once per second while
the lobby is visible. Followers use the leader's reported elapsed time. Ranked shows
elapsed time and the server-provided range; stale data shows a checking message.
The timeout route uses a dedicated native search-cancel graph, preserves mode and
party, and follows native Back animation. Public suppresses the Ranked timeout/range
route. Confirmed rooms hide the search panel and use the native countdown.

The passive panel reuses native countdown artwork and anchors, original text rotation,
fitted text boxes and the native traversal chain. Generated assets reuse the preparation
cache, including invalidation and stale-output removal.

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
project and the guest remains waiting. Ranked/Public/Ranked re-entry preserves the party.
Private focus rearms the lifecycle and retains its GUI handle for the native
`CGuiEventPrivateLobbyClose` route. This avoids treating an active Private session as an
already-returned guest and avoids the scene reload fallback. Repeated Private entry/exit retains the native close route. The Private close route also updates the registered menu name at
race manager `+0xdf0` to Waiting Room before dispatch, matching native enter-lobby lookup.
Private-to-Public/Ranked re-entry uses the same registered menu destination.

Lobby graphs replace `CNetworkNodes::stopMatchMaking_VSC` and Private's `disconnectAll_VSC`
with `resetNetworkSessionManager_VSC`; the Private reset alias is normalized too. The named
matchmaking stop calls the asynchronous disconnect path, whose callback at `0xa181e0`
resets the party. Its reset call at `0xa181fd` clears connected peers without delivering the expected
return notification to the guest.
Unrelated native sync-stop functions and their call sites are left unchanged.

Menu lobby-closed status and native UI cleanup remain. Main's explicit offline-exit graph,
explicit Leave Party, and network kick/disconnect retain their native behavior. PSN
race-session cleanup preserves host-follow state and session records while native
`hasActiveParty_VSC` transport count remains positive; race roster size is not sufficient.
Old status replies cannot revive cancelled searches, and polling stays suspended until
lobby re-entry. Both update2 and Online-domain graph copies use the existing preparation
cache with source invalidation. Menu transitions retain the party; explicit Leave Party and offline cleanup keep their
separate native behavior.

## 4. RACE-END REPORTING

Every client submits its view of the race through signed `POST /ryunet/ranked-result`.
The payload contains `v:2`, room, mode, track, and an `order` array with each original
human's `name`, `pos`, `dnf`, `time`, and `dc`. `self` contains only the original local
human's `finished`, `time`, and cumulative lap timestamps. See
[RANKED_ENDRACE_DESYNC_FIX.md](RANKED_ENDRACE_DESYNC_FIX.md) for capture and trust rules.

Identity is cached incrementally during the race and frozen before a departure becomes
native AI. Rehost must not change names to character names, overwrite race IDs with local
join slots, or mark transferred remote racers as `(me)`. Survivors report the departed
human with `dc:true`; the departed client does not need to submit its own report.

Responses can be `pending`, `resolved`, or `void`. The client must not assume the
host reports first or that a result is immediately available. Server validation and
consensus policy are documented in RyuNET's `docs/RANKED_ENDRACE_DESYNC_FIX.md`.

The client worker posts at most eight times, with 3-second waits between retries, and
accepts responses up to 16 KiB. Server wait/network time adds to this budget. It stops on
a resolved result or a nonzero non-200 HTTP status. Response generation and mode checks
prevent an old reply from changing a newer result card. There is no guaranteed instant
MMR result: missing reports can require the consensus window.

Resolved responses freeze `race`, `season`, `mode`, `delta`, `delta_applied`,
`ranked_before`, old rating/tier when qualified, placement progress, and the new rating/tier
when qualified. `participants` includes each racer's position, DNF/DC, delta and placement
state. Placement responses without public MMR are valid. Retrying returns the same race
snapshot rather than a later profile. Expired-season reports are rejected.

## 5. AUTHORITATIVE RESULT DISPLAY

The client does not calculate MMR. Display `delta_applied` when present, otherwise
`delta`, and use the response's placement and old/new tier fields. Zero is a valid
applied delta. Never substitute a locally estimated award or penalty.

Scoring formulas, penalty settings, tier eligibility and season rollover are defined
in the server repository's `docs/RANKED_MMR_V5.md` and backend specification.

## 6. TIER DISPLAY

The client maps the server-provided tier name to the corresponding native badge.
Eligibility and MMR thresholds are server-owned; do not infer a tier from MMR locally.

| Badge | NORMAL | ITEMLESS |
| --- | --- | --- |
| 1 | Bronze | Sapphire Relic |
| 2 | Silver | Gold Relic |
| 3 | Golden Wumpa | Platinum Relic |
| 4 | Power Crystal | Sacred Fire |
| 5 | Mythic Gem | Ultimate Sacred Fire |
| 6 | Uka's Chosen | Time Crusher |
| 7 | Ancient Spirit | Master Driver |

Lobby Members uses seven stock kart-decal sprites. Placement players display Unranked;
profiles without a fetched result do not receive a fabricated badge.

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
  server-owned range expansion; Public suppresses the Ranked timer route. Confirmed rooms use the
  original pre-race countdown.
- **Launchers:** Linux and Windows omit Ranked/MMR information. Placement progress,
  ratings, and tiers remain available in the native game UI.

### Background artwork and preparation

`scripts/assets/ranked_background.png` and `scripts/assets/hof_background.png` use the
native Matchmaking source orientation. The texture compiler flips PNGs vertically before
BC3 encoding; do not apply an additional flip or rotation when editing the reference.
The native pattern quad has no rotation or negative scale; Matchmaking and Private use
identical animation curves. Custom materials retain the native animation.
Both updated PNGs are 256×256, non-interlaced 8-bit RGBA. PNG edits invalidate the existing
archive preparation cache; subsequent warm preparation reuses the output.
USA/EU texture compilation covers the background assets.

UI regression tests cover native preparation and navigation.

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

### Batch tier fetching and HOF compatibility

Batch tier lookup requests `mode=all` (`tiers0`, `mmr0`, `tiers1`, `mmr1`) and falls back
to separate mode requests for older servers. Mode switches reuse the appropriate cached
values and refresh them without mixing ratings. The client consumes the normal/itemless
profile sections without requiring country-rank or rating-history enrichment.

HOF tier validation permits apostrophes so `Uka's Chosen` does not reject the entire
Normal Everyone response. All 14 canonical tier names and invalid markup have fixture
coverage. Response parsing covers both Everyone boards without rejecting valid tiers.
