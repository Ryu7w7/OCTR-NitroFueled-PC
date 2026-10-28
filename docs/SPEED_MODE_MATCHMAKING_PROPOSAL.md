# Proposal: Speed Mode matchmaking for RyuNET

Hi Ryu, I would like to propose a separate public **Speed Mode** race queue, with its own MMR if ranked play is added later. I have tested Speed Mode in a private match with another player, including a match without AI. The current implementation is experimental and requires every participant to use a compatible SacredFire build with Speed Mode enabled.

## Proposed public behavior (not implemented)

- The native Game Mode selector offers **Normal Mode / Speed Mode / Battle Mode**. Players who choose public matchmaking with it enabled are matched only with other Speed Mode players. Normal Race and Battle matchmaking remain separate.
- Speed races have no item boxes, Wumpa boxes, item or Wumpa HUD, or seasonal ghost/Golden Wumpa pickups. Shortcuts that normally require an item should be usable without one.
- Adventure and Arcade keep their normal boxes and HUD. Private Speed matches continue to work independently of the public queue.
- An incompatible client cannot join a Speed lobby, and a Speed client cannot enter a normal public race with Speed assets active.

## Suggested server/client contract

These message names are proposals; they should be adapted to the matchmaking protocol RyuNET already uses.

1. The server advertises a versioned Speed matchmaking capability, for example `speed_matchmaking: 1` in `/ryunet/theme`. Until it does, public matchmaking keeps normal race assets even when Speed Mode was selected for a private match.
2. When a player starts **public matchmaking**, the client sends `race_mode=speed` and its Speed content version in the authenticated matchmaking request. The server puts the player in a separate Speed queue. A login-time preference alone is insufficient because a player can enter and leave different game modes during one session.
3. Before loading a public race, the server confirms the lobby's race mode and compatible content version to every participant. The client activates Speed assets only for a confirmed Speed lobby or a private Speed room. If mode confirmation fails or conflicts, the client cancels the join instead of entering a mixed-rule race.
4. The server enforces mode separation for lobby creation, joining, migration, and rematches. It should reject incompatible clients rather than silently falling back to the normal queue.

## MMR, after queue separation works

- Keep Speed MMR separate from normal Race and Battle ratings.
- Calculate it on the server from authenticated **completed public Speed races**. Private matches, AI races, and abandoned sessions should not grant rating.
- Define handling for disconnects, ties, parties, and small lobbies before enabling rating changes. An unranked Speed queue can launch first.
- Return rating and queue status through a server API for the Linux and Windows launchers to display. The client should never submit its own authoritative MMR.

## Current client state and remaining work

The current Speed build generates normal and Speed archive views from the player's own game files (`scripts/speed_mode.py`) at launch. `src/runtime_file.c` selects the Speed view when Speed Mode is selected in the native Game Mode selector and a private WebAPI room has recent game traffic with another player (`src/runtime_services.c`, `src/runtime_net.c`). The native selection applies without restarting. Public matchmaking uses the normal view until the server supports a separate Speed queue. The launcher and ImGui menu no longer expose a Speed toggle.

The USA 1.21 client adds **Normal Mode / Speed Mode / Battle Mode** to the existing native **Game Mode** selector shared by **Matchmaking** and **Private Match**, before entering the lobby (`scripts/online_mode.py`). It does not add a dialog at Online entry. The runtime adapter (`src/runtime_online_mode.c`) maps the private Speed choice to the existing race event while enabling the session's Speed flag. Normal and Battle clear that flag. Returning from a private lobby restores the selected Speed mode; visiting public matchmaking does not erase that private selection. Speed is disabled for public matchmaking until a separate server queue is supported. Native initialization and ordinary selector navigation remain in the game. The patch is generated from the player's archive and validated against the supported asset and executable layout; it does not synchronize private players' selections.

For public matchmaking, the client still needs a signal for the **server-confirmed public lobby mode** and a compatible-build check. Seasonal ghost/Golden Wumpa suppression now follows the same active-race predicate as the Speed archive view (`runtime_speed_race_active`). The native hidden-object manager receives mode 0 for an active private Speed race and the incoming seasonal mode for other races, evaluated on each level load. Returning to Normal restores the configured/server theme without restarting; remembered private Speed selection alone does not suppress it outside an eligible room. The private match tests do not establish that public queue isolation or MMR works yet.

Suggested first acceptance test: one Speed client and one normal client search at the same time and never enter the same lobby; two compatible Speed clients match, race with no item/Wumpa boxes, finish, then return to Adventure/Arcade with normal boxes present.


## Developer handoff: enable public Speed after the protocol is ready

Public Speed is currently disabled. Search the code for `PUBLIC_SPEED_STEP_` to
find the integration comments. They contain **commented examples**, not a hidden
working implementation. Uncommenting them alone will not compile: the proposed
APIs below do not exist yet. There is intentionally no client-only toggle that
sends Speed players into the normal queue.

The native UI is already working: **GAME MODE → NORMAL MODE / SPEED MODE /
BATTLE MODE**. The user verified private Speed selection, returning from the
lobby without losing the selection, and leaving Online without crashing. The
launcher and ImGui toggles are removed in source; the live test used the existing
ImGui library because rebuilding it was explicitly deferred. No public Speed
race or queue isolation has been tested.

### 1. Negotiate capability and expose the option

In `src/runtime_online_mode.c`, `selector_event()` currently enables option 1
only in Private Match. `PUBLIC_SPEED_STEP_1` shows where a negotiated capability
can enable it for public matchmaking. Keep this disabled until the client also
implements the server's actual queue protocol and compatible content version.
Do not reuse `private_speed_selected` for public selection: returning from a
private Speed lobby must not implicitly choose a public Speed queue.

### 2. Carry the requested mode into matchmaking

`select_mode()` is a UI callback. Index 0 is Normal, 1 is Speed, and 2 is Battle.
`original_choice(sender, index == 2 ? 1 : 0)` maps these onto the native game's
Race/Battle events. **Keep that mapping.** It does not identify a server queue.

`PUBLIC_SPEED_STEP_2` sketches recording a public Speed request. Send the selected
mode and content version in the authenticated request that actually starts a
search after **Go To Lobby**. Implement the server contract above for searches,
party joins, invitations, lobby migration and rematches. A rejected or unsupported
Speed request must stop and show an error; it must not silently retry as Normal.
Keep requested selection separate from confirmed lobby mode.

Suggested API boundaries (names are examples; implement them in the existing
network/session layer and declare them before using the snippets):

| Proposed API | Meaning |
| --- | --- |
| `int runtime_public_speed_available(void)` | The current server advertises a supported protocol/content version and this client implements its isolated queue. Default false; clear on server change or disconnect. |
| `void runtime_public_speed_request(int enabled)` | Remember the public queue choice for the upcoming search. This never authorizes Speed asset loading. Clear on cancel, leaving public matchmaking, or selecting Normal/Battle. |
| `int runtime_public_speed_confirmed(void)` | The current authenticated lobby is confirmed Speed with compatible content. Default false; scoped to a specific lobby/session, not just the last response received. |

Ignore late responses from an earlier search/lobby. Clear confirmation when
leaving, disconnecting, failing to join, migrating, or replacing the lobby. A
new owner/session must be confirmed again before race assets are loaded. Apply
confirmation consistently to every participant, including late joiners; a local
checkbox or selection is not authority for a public race's rules.

### 3. Activate the appropriate archive view

In `src/runtime_file.c`, `runtime_speed_race_active()` currently requires Speed
selection and an active private room. Both `active_archive_patch()` and the native
seasonal hook use it. `PUBLIC_SPEED_STEP_3` shows the proposed room predicate:

```c
runtime_private_room_active() || runtime_public_speed_confirmed()
```

Keep the existing archive availability and mode-enabled checks. Synchronize the
runtime mode with the confirmed public lobby before loading its race assets;
clear it appropriately when leaving. A capability response alone must never
activate Speed assets in a public race. Keep Adventure, Arcade, Normal public
races and Battle on their normal asset views.

`scripts/speed_mode.py` already prepares paired normal/Speed archive views with
matching offsets. `PUBLIC_SPEED_STEP_4` marks the legacy public-button guards:
the Speed view currently strips callbacks from two public-entry graphs. Preserve
those callbacks for the supported public Speed path once queue isolation works,
then regenerate the overlays. Seasonal ghost/Golden Wumpa suppression already
uses the shared active-race predicate; enabling confirmed public Speed there
will cover both assets and seasonal mode. Keep the incoming theme intact for
Normal/Battle, Adventure and Arcade.

### 4. Acceptance checks before enabling public Speed

- With an old/unsupported server, public Speed stays unavailable; private Speed still works.
- A Normal client and a Speed client searching together never share a public lobby.
- Two compatible Speed clients can search, join, race without item/Wumpa boxes,
  finish, and rematch with the same confirmed mode.
- Wrong content versions, missing mode confirmation, party conflicts, and rejected
  joins stop the Speed join visibly rather than entering a Normal race.
- Cancel, disconnect, reconnect, migration and late server responses cannot retain
  stale Speed confirmation or affect a different lobby.
- Return from the lobby preserves the appropriate private/public selection;
  Normal/Battle changes clear Speed intent. Leaving Online remains stable.
- Adventure, Arcade and Normal/Battle public matches keep normal assets after a
  Speed session. Verify seasonal pickups as well as item/Wumpa crates and HUD.

Enable public Speed only after these checks pass on both client and server.
MMR remains a separate follow-up after queue separation and race validation work.


## Seasonal runtime validation

`tests/test_online_mode.c` executes the installed seasonal hook and its original-code
trampoline against a controlled manager stub. It checks default, Halloween and
Golden Wumpa modes through Normal → Speed → Normal, plus remembered Speed outside
an active eligible room. Unknown seasonal function signatures reject installation.
These checks pass; a full in-game seasonal race after this change still needs
visual verification. No launcher/ImGui rebuild is required for this runtime change.
