# Host migration: implemented transport and race identity

This replaces the earlier in-place role-flip proposal. See [implementation status](HOST_MIGRATION_PROGRESS.md)
and [race reporting](RANKED_ENDRACE_DESYNC_FIX.md).

## Scope and authority

Public and Ranked use the server's found-room host, which can differ from the
premade party leader. Private party-session ownership is a separate path from found-room promotion.
HTTP polling stays on worker threads. Lobby and race frames drive native work.

The client uses the host supplied by the found-room update. It does not elect a
successor from the premade leader. Server election and room retention are documented
in RyuNET's backend specification.

## Native reconnection

1. The successor creates a real native host session. A same-room promotion during
   a race uses the room update path rather than the ordinary create path, which
   enters lobby state 8. Native session work waits for the manager to become idle.
2. Guests join the successor using the room's existing credentials. Ordinary joins
   also wait for pending native HostSession work; otherwise its late completion
   can replace the connection just joined.
3. Both roles run the native race connection bind at `0xa44190`. This performs
   peer cleanup and object ownership rebinding. Merely replacing the replication
   connection left guests with heavy steering, broken jumping/items and stale rosters.
4. Departure cleanup targets the departing PID. Replacing a transport must not
   remove every participant formerly reached through that transport.
5. AI catchup updates host authority and required peers without discarding the
   race's accumulated bonuses or results.

This uses native transport creation and ownership operations. It does not flip
connection role bytes or synthesize a connection-ready event without a connection.

## Preserve race IDs across native reconnect

The native reconnect path re-registers local players and can replace their active
race network IDs with controller/join slots. That creates duplicate IDs, corrupts
result permutations and can crash podium processing.

- `0xc23120` teardown copies player `+0xb0` to `+0xc8`.
- `0xbd0ad0` re-registers local players. The actual
  join-slot write at `0xbd0cad`: player `+0xb4` is copied to `+0xc8`.
- The hooks save and restore valid local-player race IDs around those operations
  during an active public race. Native registration still runs.
- The speculative `0xc21f70` guard is not the solution and was removed.

Per-race human identity is captured incrementally: original PID, network ID,
nickname and local `self` ownership. Departure freezes finish/lap evidence before
native conversion to AI. Ownership transfer must not turn a remote racer into a
second `(me)` or replace its name with the AI character name.

The native result handler still runs if custom permutation validation rejects an
order. Swallowing that handler suppresses the Public podium. Ranked intentionally
uses its own results flow and returns to the online menu.

## Disconnected human karts

Only a racer recorded as a human and subsequently marked DC receives these rules:

- AI actions, steering and horizontal velocity are stopped; gravity remains active.
- Dynamic entity contacts are filtered when either side is the departed kart.
  Static ground contact remains intact.
- Kart, driver and four wheel entities blink for 400 ms visible, 400 ms hidden.
  Visibility is restored when the current racer no longer qualifies.
- Detection resolves current racers against cached identity, not just stale entity
  pointers or reusable slots. Ordinary matchmaking AI and replay playback are excluded.

## Room and party lifecycle

Room departure is distinct from premade departure. Post-race notifications must
not claim a retained party member left. Re-registration retains the capability
nonce used by a pending confirmation. Old return/update/cancel packets must not
cancel a replacement search or overwrite a different room.

Public followers clear stale Private state. Leaving a room releases its native
roster, peers and countdown even after racing. A lone survivor retains the room's
waiting/chat context; a premade alone is not an outside opponent. Join failures retain the client’s ability to retry using refreshed room state.

## Verified native facts and rejected assumptions

- Connection `+0x110` is the host peer, not a session pointer.
- `0x4082b0` initializes the connection role at `+0x118` while `+0xf0` is clear.
- Event `0xe` reports real connection initialization/teardown. It cannot itself
  establish transport or ownership.
- `0x1538380` and `0x152ec50` are IGB file I/O functions, not network role setters.
- `0x3ee5a0` / `0x4332f0` implement conditional object ownership transfer;
  their existence does not prove all race objects support arbitrary transfer.
- `0xa57350` installs a new session and emits LobbyBecomeHost; it is not proof
  of a safe in-place role flip.

Earlier claims that migration was impossible, that all observed crashes shared
one proven allocator cause, or that allocator recovery was the only remaining
option are superseded. Do not restore those assumptions as implementation guidance.

## Regression coverage

Fixtures cover 2–8 racers, host and guest departure, sequential departures,
controller-slot changes, original AI exclusion, collision/blink state and new-race
reset. Reporting verifies stable original identities and sender-only finish evidence.
