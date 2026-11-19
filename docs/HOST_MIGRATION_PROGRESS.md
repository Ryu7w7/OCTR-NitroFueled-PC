# Host migration via server — progress

Work stages, based on data (not assumptions). Update this file whenever a step is done.

## Key findings (data)

- All crashes and host-takeover events happen in **private rooms** (`menu return role=1 private=1` in the logs).
- Ranked/public (`private=0`) has a server room (`ctrRoom`), but it is not the crash target; host-leave there does not crash.
- **The RyuNET server had no `/v1/sessions` endpoint** (grep: 0 results at the time). The private-room session
  `cusa13795-session-<host>` is fully emulated in the client (`runtime_services.c`) through
  `sessions/members-<sid>.txt`, format `name|accountId`, max 8.
- Root cause of the crash: host quit → session delete → guest native party **teardown + recreate** →
  TLSF allocator corruption (`0x1490000`, `0x1590e5d`, `0x1590b82`). Guards only moved the fault address.
- The binary has in-place role-flip primitives (`0x1538380`, `0x152ec50`), but calling them from a
  server notice has never been verified live.

## Architecture decision

Build **server-side party sessions** in RyuNET so the host can be promoted before leaving:
the session is not deleted when the host exits → guests never receive a session-delete → no
teardown → no corruption.

## Stages

### Stage 1 — Server: party session model + endpoints + tests  [x]
- [x] `partySession` struct: id, owner conn, ownerPID, members map, generation, created.
      (`server/party_session.go` — keyed by nick, not conn, because the client emulates the party
      per nick; sessionId = `cusa13795-session-<host>`).
- [x] Store with mutex, separate from `sessions`/`ctrRoom` (`partySessions` + RWMutex).
- [x] Endpoints:
  - [x] `POST /v1/sessions` — host create, returns sessionId.
  - [x] `POST /v1/sessions/<sid>/members` — join.
  - [x] `GET /v1/sessions/<sid>/members` — list.
  - [x] `DELETE /v1/sessions/<sid>/members/me` — leave (host with no members = delete session).
  - [x] `POST /ryunet/party-promote?sid=<sid>&to=<nick>` — transfer owner + generation.
        (No client notice yet at this stage — the endpoint returned the new sessionId + generation.)
- [x] Route registration in `auth.go`.
- [x] Unit tests: create/join/promote/leave (`server/party_session_test.go`, 5 tests).
- [x] `go test ./...` passed: **364 passed** at this stage (360 old + 4 new).

### Stage 2 — Client: server-backed private session (env-gated)  [x]
- [x] Env var `BB_PARTY_SERVER=1` enabled server-backed mode (`runtime_party_server.c`).
- [x] `runtime_party_server_create()` called from the `POST /v1/sessions` interception
      (`runtime_services.c`) — mirrors the party to the server.
- [x] `menu_return`, role==1 private room: call `runtime_party_server_promote(successor)`
      before the native quit; successor picked from the roster (`runtime_room_members`).
- [x] Guest: `runtime_party_server_tick()` polls `GET /v1/sessions/<sid>/members` on the lobby
      frame (`runtime_public_itemless_update`), detects generation/owner changes.
- [x] **Race safety (user requirement):** no native state touched while a race runs.
      - promote during a race → `runtime_party_server_defer_promote()` (store, no request),
        applied on the first tick after the race ends.
      - guest role flip during a race → deferred (`pending_promote`), applied after the race.
      - polling still runs during a race, but read-only HTTP.
- [x] Default off at this stage (no `BB_PARTY_SERVER` = exact old path).
- [x] Race-safety test: `tests/test_party_server.c` — 1 test, passing (deferred promote sends no
      request during a race, only after race end).
- [x] `out/sacredfire` build OK; `public-itemless`, `party-server`, `online`, `room` tests pass;
      `git diff --check` clean.

### Live test 1 (13:00) — findings & fix
- Second promote got 403, join got 404 → root cause: the server changed `session.ID` when the
  owner changed (map re-key). Fix: keep the ID stable; only `Owner` + `Generation` change
  (`party_session.go`). Server suite 364 pass, binary restarted.
- Live test 2 (13:05): promote 200 consistently, 404 gone. **New problem:** every takeover
  printed `Party server: created ...` again (the `POST /v1/sessions` interception fired again),
  resetting the server generation → state desync → native reload on rejoin.
  Client fix: dedupe `create` (skip if a session is already active), track `owner_name` so the
  old host stops promoting after losing host (403), and read the session id from the join
  response. `test_party_server.c` updated (assert create skipped).

**Pending (needs live):** the native in-place role flip on the guest after a promote notice is
still not wired to the `0x1538380` primitive — the module only records intent (log). Another
live test is needed to confirm dedupe + owner tracking remove "loading on rejoin".

### Stage 3 — Stabilization  [x]
- [x] Repeated live tests: promote 200, create dedupe working, no crash, no loading on rejoin.
- [x] Make the server the default (remove the `BB_PARTY_SERVER` gate) — see Stage 4.
- [x] Remove the dead ranked wiring `promote_before_leave` (client call, definition and
      forward declaration; the now-unused `leaving_room` capture in `menu_return` went with it).
      The `/ryunet/room-promote` server endpoint stays — it has no client caller now, but
      removing it is unrelated to this cleanup.

### Stage 4 — Final user request  [ ]
User (13:14): private room is safe. Remaining items:

1. **Server as default** — remove the `BB_PARTY_SERVER` gate; no env/command needed. **[DONE]**
2. **Native takeover disabled** — no more `Host takeover ... through native party recreation`;
   the server is the single source of truth for host. **[PENDING — depends on the native flip]**
3. **Queue not reset** — when the host leaves in Ranked/classic Matchmaking, guests stay in the
   same queue: no switch back to Casual, no scene reload, MMR range unchanged. Applies to all
   modes. **[PENDING]**
4. **Found-room departure** — once opponents are found, if the host/guest/any player leaves:
   - enough players left (≥2, per the server rule `ctrAsync.go: minimum=2`) → keep racing;
   - not enough → leave immediately, do not hang. **[IMPLEMENTED, see below; UI still lagging]**

**Critical dependency (blocker for item 2):** disabling native takeover without replacing the
native role flip = private room dies (guest stays role 2, cannot start a race). Native takeover
is currently what performs the real role flip in the game. To disable it properly, the in-place
flip via `0x1538380` must be wired; its arguments are still not verified live. This is the only
part that cannot be done safely without a user decision.

**Order plan:** items 1, 3, 4 first (reasonable from code + data), item 2 after the native flip
is decided/verified. User chose (13:20): **disable native takeover + wire the in-place flip, do all of it.**

**Additional requirements (ranked rules, partly present):**
- Max 4 party members for Ranked queue — already in the server (`rankedCheckAdmission`) and
  client (`ranked_party_size()>4` → cancel + notice).
- Queueing 5 → native notice on host ("Ranked supports at most 4...") — already in
  `runtime_public_itemless_update` via `notice()`, needs verification when actually queueing.
- Queue of 4 then a 5th joins → auto-cancel — already present (lines 639-654).

#### Stage 4 item 4 detail (final user 13:22)
- **Ranked:** at least 6 live players (solo/cross-party alike). < 6 → back to queue.
- **Classic:** at least 2 live players in different parties (AI fills the rest). One party alone
  → back to queue.
- The game returns to the queue on its own when the server room is dissolved (user confirmed).
- Implementation: `server/ctr_foundroom.go` — `foundRoomRequeue(r, departing)` +
  `livePlayerPIDs`/`liveParties`/`isRankedRoom`, with `rankedMinRacePlayers=6`.
  5 unit tests (`ctr_foundroom_test.go`) passed at this stage, server suite 369 pass, build OK.
- **Async sweep (not `defer`)** wired: `startRematch()` 2-second ticker →
  `sweepFoundRoomsLocked()` → dissolve rooms that cannot race.
- **Live bug (13:40, server log):** a 2-solo classic match found, room=5 `open=true searches=2`,
  one player left but stayed listed and the other kept racing.
  Cause: the old sweep only ran when `lobby_open=false` (`isRacingRoom`), but a newly found room
  is still `open=true`. Fix: replaced with `isFoundRoom()` (searches>=2 or hostLeft), so the
  sweep runs from match-found onward. 6 unit tests pass, 370 pass, build OK.
- **Ranked account gate:** `RYUNET_RANKED_REQUIRE_ACCOUNT=0` added to `.env` as a live setting
  for local 2-client testing (backup `.env.before-ranked-account-<timestamp>`).
- **2-client limit:** ranked min-6 and classic 2-different-parties scenarios cannot be verified
  live; they are covered by server unit tests only.

#### Leave notification — first pass (user: "must send to the server so it knows")
- Client `leave_lobby()` sent `POST /ryunet/room-left?name=<me>&room=<matchmaking_room>` BEFORE
  `runtime_public_itemless_reset()` (which clears `matchmaking_room`).
- Server `handleRoomLeft()` (`server/ctr_foundroom.go`, route in `auth.go`): marks the leaver
  `player_state=3`, runs `sweepFoundRooms()` immediately. If the room is still raceable it sends
  `ctrUpdateLobby` (2006) to the host so the ghost member disappears; otherwise the room is
  dissolved and the game returns to queue.
- Applies equally to solo / party member / leader (all are just state 3).
- 7 found-room unit tests passed (including TestFoundRoomLeaveSoloAndPartyMember),
  server suite 371 pass, client + server build OK. **Not live-tested yet at this point.**

### Live test 3 (13:55) — the leaver was the HOST
- Server log: `CTR room 3: host pid=1821693054 left with 1 other player(s); kept 45s for a new host`
  → host-left grace in `ctr_hostleft.go`, not the found-room sweep. Guest re-registered ~20s
  later (`#2 CTR search=2 resumed`) — that was the delay the user saw.
- Root causes: (1) the notify was only in `leave_lobby`, but native Quit goes through
  `menu_return`; (2) `livePlayerPIDs` counted the host as live via `pid == r.host.pid`, so the
  sweep never dissolved a 2-solo host-leave.
- Fixes:
  - `notify_room_left()` called from **both** `menu_return` and `leave_lobby`, deduped with
    `room_left_reported` (reset on join).
  - `livePlayerPIDs()` treats the host as not live when `r.hostLeft != 0`.
  - Server sends `ctrUpdateLobby` (2006) even when dissolving, so the host leaves the found
    room right away.
- New test `TestFoundRoomHostLeftDissolvesTwoSolo`. 8 found-room tests, 372 suite pass,
  client + server build OK.

### Live test 4 (14:00) — client notification never fired
- Ranked: `14:01:10` found → `14:01:12` `[RyuNET Matchmaking] room 3: survivors cannot race
  (2 live, ranked=true); dissolving -> queue` — the 2-second sweep, not a client notification.
- Classic: room=6 reached `connected=1` at `14:01:51` with **no dissolve log** after it.
- Client logs contained **no `room-left` line at all**: `matchmaking_room` was already `0` when
  `menu_return` ran, because native Quit calls `cancel_search`/`reset` first.
- Fix: room id is now **sticky** (`left_room_pending`), set in `accept_entry()` on join,
  survives `reset`, cleared when reported. `notify_room_left()` takes no argument and reads the
  sticky id. Dedup unchanged.
- Server log line added: `[RyuNET Matchmaking] room-left room=N player=<nick> dissolved=<bool>`.

### Live test 5 (14:07) — notification works, dissolve push was missing
- `14:08:04 [RyuNET Matchmaking] room-left room=3 player=n0buholic dissolved=[]` — the client
  notification **did fire**, ~21s after found (the user had the "LEAVING LOBBY / Are you sure
  you want to quit?" confirmation open). `dissolved=[]` because the room had already been
  dissolved at `14:07:43` by the sweep (ranked, only 2 players).
- The remaining host still sat in the found room with "MATCH BEGINS IN 34" and a ghost member:
  the sweep dissolved the room on the server but **pushed nothing to the clients**.
- Fix: `sweepFoundRooms()` now returns `(dissolvedIDs, notices)` and builds a `ctrUpdateLobby`
  for each dissolved room BEFORE deleting it. `startRematch()` sends those notices;
  `handleRoomLeft()` sends its own host notice only when the room survives (no duplicate).
- Test updated to assert the dissolve push contains a 2006.
- **Log format unified:** `lobbyConn.logf` now prefixes every connection log with
  `[RyuNET Lobby] #N ...`, matching `[RyuNET Gates]` / `[RyuNET Auth]`. Found-room logs use
  `[RyuNET Matchmaking] ...`.
- 8 found-room tests, **372 suite pass**, `go build` + `bash build.sh` OK, `git diff --check` clean.

### Live test 6 (14:32) — queue timeout / leader quits while queued
- Server: `14:32:03 #2 CTR search id=1 registered; seats=2 filter=b95eee4f notifications=0`.
  `notifications=0` = the search only waited in `m.waiting`; **no `ctrRoom` was created**, so
  the leave sweep and `room-left` endpoint had nothing to act on.
- Client logs: cup-test was the search leader (`starting RANKED NORMAL search`) and quit
  (`menu return role=1 private=0 before=2`) but emitted no `room-left` — the sticky room id is
  only set in `accept_entry()`, which a search leader never reaches.
  n0buholic never started a search; it was the party guest (`role=2`, state 7). When the leader
  left, `Party lifecycle: reset ... state=7 role=2 ... menu_returned=0` fired but **no
  `menu return private=0`** followed, so its found-room UI stayed on screen (the screenshot).
- Root cause: `runtime_party_experiment.c:230` only notified the host (`role == 1`) on roster
  shrink; the orphaned guest got nothing and no UI transition closed the screen.
- Fixes:
  - UI-root detection in `native_return_focus()`: track the previous lobby graph and treat
    ModeSelect/Main as a room exit **only** when the previous view was a room/queue graph.
    `OnlineMode -> ModeSelect` is forward navigation, not Back, and must not notify.
  - `runtime_party_experiment.c`: on roster shrink with `last.role == 2` (orphaned guest),
    non-private mode and an active matchmaking state, show the translated party-left notice
    and call `runtime_public_itemless_menu_return()` so the UI actually leaves the screen.
- **Timeout constraint preserved:** the 180s path (`update_search_clock` →
  `search_timeout_pending=1` → `runtime_public_itemless_update` → `queue_return_pending=1` +
  `runtime_online_mode_matchmaking_cancel`) is untouched. `menu_return` still calls
  `native_party_return(party)` for a host with `before > 1`, so the whole party returns together,
  and `online_return_button` still targets `OCTR_RANKED_BUTTON` vs `OCTR_HALL_OF_FAME_BUTTON`.
  The guest-side menu_return is gated on `runtime_public_itemless_match_active()`, which is false
  after the timeout reset, so it cannot disband a party on a search timeout.
- Build OK, `git diff --check` clean. Not live-tested yet.

### Live test 7 (14:55) — requeue must be immediate and keep the mode
- Server log: `14:55:44` room=5 found (2 solo searches, ranked filter), `14:55:45` sweep dissolved
  it, but the 2006 push went **only to the host** (`#2 CTR matchmaking push event=2006`,
  `#2 CTR sync refused: room 5 is gone`). The guest (`#1`) got no push and only re-registered
  at `14:56:07` — ~22s stuck in the found-room UI.
- Test 1 verdict: the remaining player must **continue queueing in the same mode**, not be sent
  back to the menus. Test 2 (timeout 3:00) and Test 3 (forward navigation) passed. Test 4
  (found-room departure) still showed the delay.
- Fixes:
  - `sweepFoundRooms()` now pushes `ctrUpdateLobby` to **every player** in the dissolved room
    (dedup by conn), not just the host. Updated the dissolve test to accept multiple pushes.
  - Removed the guest-side `menu_return()` added in live test 6 — it sent the survivor to the
    menus. The game re-issues `start_search` itself once it sees the room is gone.
  - Mode-preserving requeue: `joined_itemless` snapshots the found room's variant alongside
    `joined_ranked`; `runtime_public_itemless_takeover_host()` restores both before the native
    re-search. Classic Normal/Itemless/Battle and Ranked Normal/Itemless keep their mode;
    Battle also travels in the native `raw` search payload.
- 372 suite pass, client + server build OK, `git diff --check` clean. Not live-tested yet.

### FINAL design (user, after repeated requeue failures): cancel + return to ModeSelect
- Requeue kept failing in two ways: mode fell back to Casual Normal, and the timer carried over
  from the previous search (~40s immediately). User chose the simple, deterministic path instead:
  **when anyone leaves a party while still queueing/found room, cancel the search and send every
  screen back to its ModeSelect; the party must not disband.**
- Role split: the promoted host (`runtime_room_mode_takeover()` clears `followed_host`) returns to
  ModeSelect via `runtime_online_mode_matchmaking_cancel(route)` (route 1 ranked / 0 matchmaking).
  Party members still following stay in the party at "Waiting on host..." (just `party_ready()`).
- `runtime_public_itemless_abort_queue()` sets `queue_abort_pending`; `do_queue_abort()` in the
  main tick performs the cancel/reset/party_ready. Triggered from the shared-session owner-change
  (`runtime_party_server.c`) and from `/room-alive` when a found room dissolves.
- All requeue machinery (saved raw/result/self, `save_search`, `runtime_public_itemless_requeue`)
  removed as orphaned. Native takeover stays ON for private rooms; matchmaking host loss goes
  through the session + abort path.
- Build OK, `git diff --check` clean. Not live-tested.

### Shared party session while queueing (user request)
- The user asked why the queueing party cannot use the same server-backed session as private
  rooms. Before a `ctrRoom` exists there is no server host to detect a native quit, and
  `runtime_party_host_lost()` deliberately skips takeover on `announced_return`, so the
  survivor can sit with no host.
- Implemented: `start_search()` mirrors the party into a server session via
  `runtime_party_server_matchmaking(host, host_name)`. The host creates
  `cusa13795-session-<host>` (deduped against the private-room session); guests join the same
  deterministic id and poll it in `runtime_party_server_tick()`.
- When the session owner changes away from the host we were following, the guest calls
  `runtime_public_itemless_requeue()` once — re-issuing `start_search` with the saved raw
  payload and mode. No native role flip during a queue.
- `runtime_public_itemless_requeue()` is shared with the `/room-alive` poll, so both the
  pre-found queue and the dissolved found room use the same immediate requeue path.
- **Live test (15:29) — requeue logged but never happened:** the client printed
  `found room 4 gone, requeueing immediately` ~25 times but no `starting RANKED ... search`
  followed. Cause: `runtime_public_itemless_requeue()` bailed on its `busy` guard — a found
  room leaves `match_found`/`searching`/`matchmaking_room` set. Fix: call
  `runtime_public_itemless_reset()` first, then `start_search` with the saved raw.
- Build OK, `git diff --check` clean. Not live-tested after this fix.
- **Live test (15:35) — queue path never signalled:** the 15:34 client logs showed no
  `found room ... gone` and no `queue host ... left`. Cause: on a queue quit `notify_room_left()`
  bailed because there is no `ctrRoom`, so nobody told the shared party session. Guest kept
  polling a session whose owner never changed. Fix: `notify_room_left()` now calls
  `runtime_party_server_matchmaking_leave()` when there is no found room — `DELETE
  /v1/sessions/<sid>/members/me` — so the server promotes the survivor (generation + owner
  change). Guest poll detects the owner change even when it becomes the new owner (removed the
  `!is_host` guard) and requeues. Rebuilt; not live-tested.

### Architectural direction (user, latest)
User rejected further hook hunting and proposed: run ranked/matchmaking through the same
**server-backed party session** used by private rooms, with the found-room session created by
the server. Then leaving is a real `DELETE /v1/sessions/<sid>/members/me`, the server is the
source of truth, and the other clients see it on their next poll — no native hook guessing.

Grounded constraints before this can be done:
- The "send on leave" part already works (`room-left` log at 14:08:04). The missing part is the
  **UI**: Lobby Members on the found-room screen is drawn by the native game from
  `sessions/members-<sid>.txt` (local), not from the server.
- Planned shape (not implemented):
  1. Server creates a party session when a found room opens (reuse `server/party_session.go`).
  2. Client joins it in `accept_entry()` (reuse `/v1/sessions/<sid>/members`).
  3. Extend `runtime_party_server_tick()` to rewrite `sessions/members-<sid>.txt` from the
     server member list (needs a new writer in `runtime_services.c`; today it only has
     `record_session_member` / `forget_session_member` for the local user).
  4. Leaving = `DELETE /v1/sessions/<sid>/members/me`; the next poll (~1 frame) drops the row.
- This is a larger cross-repo change touching the currently working found-room path. It is
  testable with the user's 2 clients (one leaves, watch the other's UI).

## Notes

- The client's `promote_before_leave` wiring was removed: private rooms never used it
  (`matchmaking_room == 0`), and found rooms are now handled by the leave notification +
  found-room sweep. The `/ryunet/room-promote` endpoint (`ctr_promote.go`) remains on the server
  with no client caller.
- Native in-place role flip (`0x1538380`) still pending and unverified live; it blocks disabling
  native takeover.
- **Ranked countdown / room-short (latest):**
  - Server `sweepFoundRooms()` no longer dissolves a newly found ranked room merely because
    it has < 6 players. It dissolves only when a player actually leaves (departure / hostLeft)
    or when the client reports short at the countdown (`rankedShort`). This lets a found room
    keep accepting players until the countdown ends.
  - New endpoint `POST /ryunet/room-short?name&room` marks `room.rankedShort = true` and runs
    `sweepFoundRooms()` to dissolve short-handed rooms.
  - Client `runtime_public_ranked_short()` polls `/ryunet/room-alive` at countdown <= 10s; if
    `players < 6`, POSTs `/ryunet/room-short`. Server dissolves + pushes 2006 to all players,
    who return to queue. Server `lobby_countdown` hook cancels locally + returns so the race
    does not start.
  - `runtime_public_found_room()` added: `matchmaking_room != 0`.
  - 372 server test pass, client + server build OK, `git diff --check` clean. Not live-tested.
- This file is the source of truth for progress; update here, not in the memory event log.
