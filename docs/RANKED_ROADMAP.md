# RANKED ROADMAP

The queue separation and the ladder core are shipped. This roadmap orders everything else,
largest value first. The client architecture is specified in
`docs/RANKED_SYSTEM_ARCHITECTURE_SPEC.md`.

## Shipped

* **Queue separation** — server-owned `octr_ranked` search filter, `octr_ranked_v1` room
  confirmation, signed party preflight. (client f4e2d9b)
* **Ladder core (v1)** — signed finish-order reports, host-authority scoring,
  void without a host report; Solo FFA deltas with placement K; DNF/DC
  penalties; two ratings (Normal / Itemless); tiers with the top-100 gate for tier 7; monthly
  seasons with archived final ladders and a soft reset (halfway to 1000, placements restored);
  `ranked-result` / `ranked-profile` / `ranked-leaderboard` endpoints; the lobby-card delta;
  the launcher's Ranked row; opponent tier icons in Lobby Members, using stock kart-decal sprites
  fetched through `ranked-tiers`.

* **Hall of Fame — completed and verified in game.** The native leaderboard supports
  Normal/Itemless, Everyone/Friends,
  and season selection, with tier names, tier badges, and the local player's position.
  Rendering, navigation, scrolling, refresh, and returning to Online are confirmed working.
  C regressions and real-game asset tests cover the implementation.

## Current work

* **Season admission gate — implemented, live QA pending.** The Ranked menu remains accessible;
  Go to Lobby is dimmed and Confirm waits for `ranked-status` before transitioning, in both
  modes. Retry uses `retry_after`; status failure fails closed. A season change invalidates
  cached profiles. Active races and their result submissions are unaffected. C regression
  tests cover the parser, retries, invalidation, deferred entry, and cancellation.

## Next

1. **Visible ranked (client-facing polish)** — the results screen marks each row with its
   delta and tier once the race resolves (the lobby card already does); a placement-progress
   banner during the first five matches; the Windows launcher gets the same Ranked row as GTK.
2. **Season life** — season-end rewards (a badge per final tier, maybe wumpa), a season-open
   announcement, and season and match administration.
3. **Integrity hardening** — strengthen race-result verification and abuse prevention.
4. **Team queues** — 4v4 and 2v2v2v2 ranked: the matchmaker learns team-shaped searches, the
   confirmation string carries the format, scoring switches to driver points (10/8/6/5/4/3/2/1)
   with team outcomes (+30/−30, +35/+15/−15/−35) and the team disconnect penalties
   (−50/−70). Same two ratings, per the spec.
5. **USF lap bonus** — the native replay sampler already tracks boost/reserve continuity per
   racer; detect a maintained Blue Fire across a full lap in ranked Itemless and add the
   spec's +5.
6. **History and administration** — expose per-season and per-player match history to
   authorized administration tools.
7. **Discord** — season standings posts, a player's recent form, and linked-nickname lookups.
8. **Cross-season stats** — per-track win rates, head-to-head records, and longest streaks
   on a public ladder page.

## Deliberately out of scope

* A separate team MMR — two ratings total, forever (spec rule).
* Client-trusted MMR math — the client reports standings; only the server scores.
