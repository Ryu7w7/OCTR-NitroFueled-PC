# Ranked roadmap

Status reviewed against the current source on 2026-11-13 (Asia/Jakarta).
“Shipped” below means implemented in this branch with automated coverage. It does not
mean deployed to the production server or verified in every live multiplayer scenario.

## Shipped: shared protocol and backend

- Separate Ranked Normal and Itemless queues, server-owned filters, signed party preflight,
  per-member capability checks, and room-bound nonce confirmation. Ranked remains Solo FFA
  with 2–8-player rooms; party members compete as individual opponents.
- Two independent ratings, five placements per mode, server-owned MMR and tiers, the top-100
  gate for tier 7, and DNF/disconnect penalties. Provisional MMR stays private.
- Signed race reports, host-authority resolution, stored guest corroboration/disagreement,
  duplicate protection, and immutable result snapshots. A race without a host report is void.
- Highest-member MMR party anchor, including mixed placement parties. Matching windows are
  400/700/1000 MMR after both sides wait 0/20/45 seconds, with no hard cap after 90 seconds.
  Closest-rating preference and the existing minimum of two seats remain.
- At most four members per Ranked party and four active Ranked accounts per OS device hash
  across solo searches, parties, and confirmed rooms. Atomic reservations and matching
  cancel/leave/disconnect cleanup protect the limit. No IP restriction or localhost bypass.
  A party can still form a room without another party; scoring is unchanged.
- Signed search status for leaders and followers, exposing elapsed time, a 180-second
  unmatched-search limit, matched state, and actual Ranked range expansion. Expired searches
  cannot match. Public Matchmaking also has the timeout, without Ranked range information.
- SQLite persistence and one-time legacy JSON import, transactional scoring, full match
  history, seasonal archives, configurable 30-day seasons, a 15-minute drain, soft reset
  halfway to 1000, and restored placements. Migration is complete, not future work.
- Profile, result, batch-tier, season-status, and game leaderboard APIs, including
  Normal/Itemless, Everyone/Friends, archived seasons, and personal standing.
- Website Hall of Fame API with 50-entry pagination, search, archived standings, and
  session-linked personal standing. Website frontend delivery is separate from this API.

## Shipped: client integration

- Ranked entry and mode selection, season-status admission gate, deferred Confirm, and
  profile invalidation after season changes. New searches fail closed when status is unknown.
- Native lobby rank card and Lobby Members tier badges, with mode-aware lookup and session
  cleanup. Rank UI assets participate in archive preparation caching and stale-file removal.
- Native Ranked result card with MMR change, tier badge, placement progress without exposing
  provisional MMR, fitted text, native animations/effects, and promotion/demotion sounds.
  Ranked suppresses automatic navigation to Wumpa; manual Next/Back and legends remain.
- Tier badges before human nicknames in Online finish/end result rows for Single Race,
  Cup, and Team Race, across Private, Public Matchmaking, and Ranked where supported.
  Normal and Itemless use their own ratings. AI, Battle, and offline rows retain native UI.
  Podium tier badges were deliberately removed; podium names remain native.
- Native Hall of Fame with mode/population/season selection, tier names and badges,
  separate fitted MMR columns for listed players and the local player, scrolling and Back.
  Existing HOF navigation has prior in-game confirmation; recent layout fixes have regression coverage.
- Elapsed matchmaking panel using the native countdown artwork and anchor, server polling
  on a background worker, 180-second native cancellation, and Ranked range messages.
  Public shows elapsed time only. A confirmed match restores the native pre-race countdown.
  Generated assets reuse the existing preparation cache on warm boots.
- Party-preserving Back between Private, Ranked, Matchmaking, and Online menus. Native
  race-lobby/UI cleanup remains separate from explicit Leave Party and offline resets.
- GTK launcher Ranked profile row, including placement progress and placed ratings.

## Implemented, live QA still pending

- Season-gate dimming, deferred entry, retries, and cancellation during an actual season drain.
- Latest result-card alignment, badge animation/SFX/VFX, DNF timing, and manual Next/Back
  across host/guest, regional layouts, and supported race formats.
- Four-account device admission and party growth before/after Ranked entry in real multi-client
  sessions, including cancel, reconnect, room changes, and host migration.
- Live retest of the search timer after the reported overflow/flicker fix: matching artwork/text
  anchors, native text rotation, larger closely spaced fitted text, and an intact traversal chain. Also verify
  party-preserving timeout cancellation, return to the previous selector with the same options,
  follower synchronization, range messages, and countdown handoff.
- Party-preserving Back from Private/Ranked/Matchmaking to Online, including host/guest
  synchronization and explicit Leave Party/offline exit afterward.
- Latest HOF selected-row contrast, row centering, and fitted columns across display sizes.

Automated evidence includes client C regressions, USA/EU native asset checks, widescreen
integration checks, the Go server suite, focused race-detector checks, and Linux amd64,
Linux arm64, and Windows amd64 server builds. The latest client build used `BB_SKIP_GPU=1`.
The server changes are on PR #3; no production deployment or server restart was performed.

## Not shipped

1. Windows launcher parity for the GTK Ranked profile row.
2. Per-opponent MMR delta labels in result rows; the local result card and row tier badges
   are implemented, but do not imply a delta column for every opponent.
3. Season-end rewards, season-open announcements, and dedicated season/match administration UI.
   Season closure API, persistence, and archives already exist.
4. Stronger result verification, report abuse detection, and anti-win-trading analysis.
   Independent corroboration is not required to score today. Device identity is client-reported
   and spoofable by modified clients; it is not hardware attestation or complete anti-cheat.
5. Ranked team formats (4v4 or 2v2v2v2), their matchmaking/confirmation protocol, and team scoring.
   Online Team Race result badges do not mean a Ranked team queue exists.
6. Ranked USF full-lap detection and the proposed +5 bonus.
7. Player/season history views and authorized history tools. Match history is already stored.
8. Ranked-specific Discord standings/recent-form delivery and cross-season analytics such as
   per-track win rates, head-to-head records, and longest streaks.

## Deliberately excluded

- A separate team MMR: retain exactly the Normal and Itemless ratings.
- Client-owned MMR calculations: only the server scores.
- IP-based Ranked account limits or a localhost exemption from the device limit.
- Podium tier badges.

## Reference

See [RANKED_SYSTEM_ARCHITECTURE_SPEC.md](RANKED_SYSTEM_ARCHITECTURE_SPEC.md) for protocol and implementation details.
Server PR: https://github.com/Ryu7w7/RyuNET-Demonware-CTR/pull/3
