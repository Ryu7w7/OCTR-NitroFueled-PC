# Host migration: root cause and target architecture

Status: DESIGN — not implemented. Read this before changing takeover code again.

## Why it keeps crashing

Every crash in logs `20261119_*` is the same root cause, at different addresses:

- `0x1490000` — virtual call through a freed lobby-player's vtable (poison value).
- `0x1590e5d`, `0x1590f46` — "bailing corrupt free", the game freeing a block twice.
- `0x1590b82` — TLSF free-list unlink writing to a corrupted `next` pointer
  (`0x5400001` = a refcount written where a pointer belongs).

All of them are downstream of one thing: **when the host leaves, the game tears down
and recreates the native party session on every remaining guest.** Each teardown
frees session / connection / roster / lobby-player objects; native callbacks retained
by the teardown touch them again; the allocator's free lists get corrupted. Guards
added at individual call sites (`object_valid` in `traced_party_reset`/`quit`, the
`0x1490000` fault skip in `probe.c`) only move the fault to the next unlink.

The pristine state (before those guards) had exactly one known site, `0x1490000`,
handled by commit `dd0b87b`. That guard was removed in `f681680` when the takeover
was rewritten to rely on native teardown — which is what introduced the churn.

## Why there is no one-line fix

The native game has **no host-migration primitive**. "Host" is whoever *created* the
Demonware session (`connection + 0x118 == 1`, set at session creation). Every path
that changes host today does it by destroying the old session and creating a new one:

- client (`runtime_party_experiment.c`): waits for native teardown, then elects a
  successor whose own teardown recreated it as host;
- server (`ctr_hostleft.go:takeOver`): only runs when `r.hostLeft != 0`, i.e. after the
  host is already gone, and only for a player who has a search of their own to host
  with — party followers do not.

The only in-place path in the native client is `CGuiEventLobbyBecomeHost` → `0xa57350`,
and it *installs a new session object* (reads one from `rbx+0xa8`, writes it to
`party+0x18`, releases the old one). That is the churn, just triggered from a hook.

## Target architecture: promote-before-leave

Principle: **never tear down the party objects of the players who stay.**

1. Host wants to leave a room with members. Before it quits the session, the server
   promotes the successor while `r.hostLeft == 0` and the room is fully alive.
2. The successor's game receives the promotion and flips to host **in place**: role
   `1`, same session/connection/roster objects, host message handlers installed.
3. The other guests keep their party objects entirely untouched.
4. Only now does the old host leave, as a normal guest (its own objects torn down,
   nobody else's).

No teardown on the staying players ⇒ no allocator churn ⇒ no corruption. It also
fixes the Ranked→Casual regression at its root: the search/mode state is never reset.

## What must exist for this to work (unverified assumptions)

These are the two things I cannot confirm from static analysis. Confirm them on a
two-instance live rig before implementing:

- **A1 — in-place host flip.** Is there a native entry point that makes the local
  player the host of an *existing* session without releasing/recreating it? If
  `0xa57350` is the only one and it always swaps the session object, A1 is false and
  promote-before-leave cannot work with the current binary — the only remaining
  option is hardening the allocator recovery in `probe.c` to cover the unlink site
  (`0x1590b82`) as well as the free site.
- **A2 — server promotion while active.** Can `ctrMatchmaker` reassign `r.host` to a
  player who is currently state 2 in that room, send `ctrCreateLobby` to the new host
  and `ctrUpdateLobby` to the others, and keep `hostLeft == 0`? The current `takeOver`
  does this only after host loss; the primitive is nearly identical and is a small,
  well-tested change to `ctr_hostleft.go`.

## Server-side change (A2) — small and independently testable

Add `ctrMatchmaker.promoteHost(roomID, successorPID)` mirroring `takeOver` but for an
active room:

- require `r.hostLeft == 0`, successor present with state 2;
- `r.host = successor`, `r.notified[successor.conn] = true`;
- rewrite `hostDoc.player_state`: old host → removed/2, successor → 1;
- rewrite `listen_server` to the successor's `info.Listen`;
- bump `r.version`, emit `ctrCreateLobby` to the successor and `ctrUpdateLobby` to the
  rest via `sendCTRNotices`;
- expose `POST /ryunet/room-promote` (signed, host-only), route in `auth.go`.

This is self-contained, has direct unit tests (`ctr_hostleft_test.go`), and does not
depend on A1. It should be implemented first regardless.

## Client-side change (A1) — gated on the live check

Originally planned for `runtime_public_itemless_menu_return` (role 1, members present):
1. call `/ryunet/room-promote` with the elected successor;
2. on success, leave as a normal guest through `leave_party_as_guest`'s path;
3. if A1 is false, keep the current native-teardown takeover and the `probe.c`
   recovery, and document that the corruption is inherent to the binary.

Status: step 1 was removed as dead wiring — private rooms never had a `matchmaking_room`, and
found rooms now notify the server on leave and let the found-room sweep decide. The server
endpoint remains, unused by the client.

## Decision needed

Confirm A1 on the live rig. If true, implement the full promote-before-leave path.
If false, the ceiling is allocator recovery hardening; the architecture change cannot
remove the crashes with this binary.
