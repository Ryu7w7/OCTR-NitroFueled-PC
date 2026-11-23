# Ranked client race-end identity and reporting

This describes implemented behavior,
not the earlier proposal for a separate result-v2 endpoint or summed lap durations.

## Report contract

Clients use signed `POST /ryunet/ranked-result?name=<reporter>`:

```json
{"v":2,"room":42,"mode":0,"track":"electron_avenue",
 "order":[{"name":"A","pos":1,"dnf":false,"time":150000,"dc":false},
          {"name":"B","pos":2,"dnf":true,"time":0,"dc":true}],
 "self":{"finished":true,"time":150000,"laps":[30000,60000,90000,120000,150000]}}
```

Names identify original humans. The payload contains 2–8 racers including the signed
reporter, with unique positions 1..N.
`self.laps` contains **cumulative timestamps**, not durations to sum.

## Client capture and host migration

`runtime_online_mode.c` captures each human incrementally as native racers appear:
original PID, network ID, nickname and whether this was the local human. It
preserves finish/lap evidence before departure converts a racer to native AI.
Reporting uses that identity rather than the AI character name or newly transferred
ownership. Only the original local human supplies `self`; a rehost must not mark
two racers `(me)`.

Native reconnect runs full race binding to repair ownership and peer cleanup.
Hooks preserve valid active local race IDs around teardown and re-registration;
the watched `0xbd0cad` write otherwise copies join slot `+0xb4` to race ID `+0xc8`.
A duplicate-ID custom permutation must not suppress the native results handler.
Public podium and Ranked's separate result navigation keep their native paths.

Survivors include `dc:true` for the original disconnected human, even when native
AI owns its kart. Normal matchmaking AI is excluded. DC is not inferred merely
from a character name or an ownership change. The client submits this flag as evidence; rating calculation belongs to the server.

## Sender and result handling

The client submits the captured outbox on a worker thread and handles `pending`,
`resolved` and `void` responses. Validation, consensus, duplicate detection and payout
rules are documented in RyuNET's `docs/RANKED_ENDRACE_DESYNC_FIX.md`.

The client sends up to eight requests with 3-second waits between retries, using a
16 KiB response budget. Network time and server holds add to that duration. It accepts
resolved placement results without public MMR and uses generation/mode guards against
stale UI updates. The UI retains its calculating state until an authoritative result arrives.

Resolved responses retain immutable per-race old/new rating, tier, placement and
participant outcomes. Retrying retrieves that result, not the current profile from a
later race. The client never calculates or rewrites the returned MMR change.

## Regression coverage

Client fixtures cover 2–8 racers, host/guest and sequential departures, slot changes,
late racer discovery, original AI exclusion, corrected local `self`, payload fields,
response parsing and result-generation changes.

See [migration architecture](HOST_MIGRATION_ARCHITECTURE.md) and
[Ranked specification](RANKED_SYSTEM_ARCHITECTURE_SPEC.md).
