# Ranked roadmap

This document separates implemented behavior from features outside the current scope.
See [Ranked specification](RANKED_SYSTEM_ARCHITECTURE_SPEC.md) for the protocol.

Server policy, scoring, admission, storage and rollout are documented in the RyuNET
repository. This roadmap covers client behavior only.

## Implemented client

- Native mode selection, season admission, lobby rank card, result card, placement
  progress and tier effects. Mode-specific badges in supported online result rows.
- HOF mode/population/season selection and MMR columns. Tier validation accepts all
  canonical names, including `Uka's Chosen`. Batch tier fetching preserves mode caches
  and falls back to separate requests for older servers.
- Party-preserving menu navigation, stable confirmation nonce, deferred native joins,
  stale return/update protection and native roster/countdown cleanup on room exit.
- Ranked elapsed/range UI and 180-second native cancellation. Public suppresses the
  Ranked timeout route; timeout policy remains server-owned.
- Host/guest native reconnect with full ownership binding and race-ID preservation.
  Original human identity, DC state and sender-only `self` survive ownership changes.
- Departed humans stop, lose dynamic collision and blink kart/driver/wheels every
  400 ms. Ordinary matchmaking AI and replay playback retain their normal behavior.
- Opt-in verbose logs; saving output does not implicitly enable frame statistics.
  Bounded name/session/path formatting and larger trophy-name storage.
- Cached native UI preparation and online race-transition regression coverage.

## Verification

Client tests cover USA/EU hooks, race transitions, identity and departure scenarios
with 2–8 racers, original AI exclusion, collision/blink state and HOF tier parsing.
## Outside the current client scope

- Per-opponent MMR delta labels in result rows; the local result card and row badges
  do not imply a delta column for every opponent.
- Ranked team queue UI and confirmation; online Team Race badges do not imply a
  supported Ranked team queue.
- Ranked USF full-lap detection and bonus display.
- Dedicated player/season history views and season administration UI.

## Deliberately excluded

- A separate team MMR: retain exactly the Normal and Itemless ratings.
- Client-owned MMR calculations: only the server scores.
- IP-based Ranked account limits or a localhost exemption from the device limit.
- Podium tier badges.

## Reference

See [RANKED_SYSTEM_ARCHITECTURE_SPEC.md](RANKED_SYSTEM_ARCHITECTURE_SPEC.md) for protocol and implementation details.
