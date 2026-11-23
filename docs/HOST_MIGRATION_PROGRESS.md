# Host migration implementation status

Public and Ranked migration preserves the active race while replacing transport,
rebinding native ownership and retaining original human identities.
See [architecture](HOST_MIGRATION_ARCHITECTURE.md) for the native hooks and invariants.

## Completed behavior

- Successor host creation and guest joins wait for native session tasks. Both roles
  perform full native race binding rather than swapping only a replication pointer.
- Active race IDs survive native teardown and local-player re-registration. The
  join-slot write at `0xbd0cad` copies `+0xb4` into `+0xc8`; hooks preserve valid IDs.
- Original PID, name, network ID and local identity remain stable after AI conversion
  and ownership transfer. Finish reports retain sender-only `self` evidence.
- Survivors report the departed human as DC. Race results remain synchronized and
  the result payload retains the DC flag.
- Departed human karts stop, ignore dynamic collision and blink with their driver
  and wheels. Ordinary matchmaking AI and replay playback are excluded.
- Public results continue through the native podium path. Ranked keeps its separate
  results flow and returns to the online menu.
- Requeue retains premade membership, confirmation nonce and mode. Old return/update
  messages cannot cancel a replacement search. Exits release old roster and countdown.

## Regression coverage

Client fixtures cover 2–8 racers, host and guest departures, sequential losses,
local-controller changes, late human discovery, original AI exclusion and race reset.
Collision tests cover both contact sides; visual tests cover kart, driver, all wheels
and restored visibility. Reporting tests retain original identities and one local sender.

## Superseded approaches

The transport-only reconnect skipped native ownership cleanup and is removed.
Connection `+0x110` is a host peer, not a session. The previously proposed role setters
`0x1538380` and `0x152ec50` are IGB file I/O. Synthetic role flips, skipped native
registration and allocator recovery are not the migration architecture.
