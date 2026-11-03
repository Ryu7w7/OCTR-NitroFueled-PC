> Current requirement (2026-11-01): the user rejected the separate SELECT TEAM
> button. Sections 8 and 14 describe an abandoned implementation. Start must
> drive the native synchronized Team Select/countdown flow. Normal/Itemless
> team scores sum finishing awards 9/6/3/1 per team. Runtime/asset changes and
> automated checks are implemented (sections 17–18). User verified selector
> entry, but reported locked team changes, item boxes in Itemless, and missing
> indicators/results. Section 18 addresses that feedback; its live outcome
> remains unverified.

# Native TEAM/SOLO for PRIVATE MATCH — research notes (1.21)

Both 1.21 dumps were decompiled in Ghidra 12.2 for this document (CTR_EU project =
`/tmp/opencode/ctr_re/eu/eboot.elf` from CUSA14876; CTR_USA project =
`/tmp/opencode/ctr_re/usa/eboot.elf` from CUSA13795). Scriptвостоки:
`ctrscan.py`, `ctr_decompile.java`, `ctr_xrefs.java`; reports in `/tmp/opencode/ctr_re/`.

TL;DR: the game ships native team selection, team count, faction replication,
Battle score HUD, and team ranking. The private-match changes should preserve
that native path: sync the host's chosen count (including FFA), enter the native
Team Select project from the lobby, and let the game's team events update host
and guest. Never force `teamCount >= 2` or open a duplicate ranking project.

## 1. Global singletons (shared USA & EU addresses)

| symbol | address | meaning |
|---|---|---|
| race_manager | `0x02acde48` | race state (repo already patches it); `+0xd40` = race mode (enum `EOctaneRaceModes`), `+0xd4c` = mode used by battle/arena checks, `+0xd90` = flag zeroed when a ranking screen opens |
| race settings | `0x02ad4e30` | **`+0x28` = teamCount** (2 → teams, 1/0 → FFA); `+0x2c/+0x30` = lives/time-limit used in arena setup |
| gui_manager | `0x02ac0dc0` | passed to `open_project(0x986820)` (same addr both regions) |
| network | `0x02acb3d8` | `+0x228` byte used by host team assignment helper |
| event bus | `0x02ad1160` | player list iterated when settings change (`+0x10` list) |

`EOctaneRaceModes` enum order (from `eORM_*` strings): 0 Invalid, 1 SingleRace,
2 TimeTrial, 3 RelicRace, 4 BossFight, 5 CtrChallenge, 6 CupRace, 7 HUB,
8 CrystalChallenge, 9 BattleArena, 10 CaptureTheFlag, 11 LastKartDriving,
12 StealTheBacon, 13 CrystalGrabBattle, 14 Sports, 15 ModeR.

## 2. teamCount — the master switch

**Setter `FUN_00bf32d0(settings, count)`** (EU 0xbf32d0; verify USA near 0xbf33e0):
```
settings->teamCount(+0x28) = count
if host:
    for each player i in event bus list: FUN_00bf36c0(player, i % count)
else if single player: same modulo assignment
```
`i % teamCount` is the native auto-balance ("team_balance") — it caps a 4v4 to
per-team slices, so 4/2/3 teams in a 8-player private match distribute evenly.
(FUN_00bf3390 handles the online-host path with the byte at `network+0x228`.)

**Menu handler `FUN_01293020`** (private settings menu item callback):
- on select event → `FUN_00bf32d0(DAT_02ad4e30, selected_index + 1)`; index 0
  ("No Teams") writes 1.
- highlight refresh (`FUN_01293170`): `item = settings->teamCount(+0x28) - 1`.
- mode-conditional visibility helpers run for modes 0xc (CrystalGrab) and 10 (CTF);
  `race_manager+0xd40` read directly here.

**Results-screen chooser `FUN_00e9ba20` (EU) / `FUN_00e9ba30` (USA)** — native
ranking project picker; both verified:
```
mode = race_manager + 0xd4c
if mode-1 in mask 0x1f39 (modes {1,4,5,6,9,10,11,12,13}):
    if local players >= 2: 'Octane_Race_Leaders' mini box
if local players < 2:
    switch(mode):
      1,4 -> 'Octane_Race_Ranking'
      2   -> 'Octane_TimeTrial_Ranking'  (+0x88 slot)
      3   -> 'Octane_RelicRace_Ranking'  (+0x80)
      5   -> 'Octane_CTRRace_Ranking'
      6   -> 'Octane_CupRace_Ranking'    (+0x60 slot; screens 9/6/3/1 already
                                              used by our apply_cup_results)
      8   -> 'Octane_CrystalChallenge_Ranking'
      15  -> 'Octane_ModeR_Ranking'
      9..13 -> race settings.teamCount(+0x28) < 2
                 ? 'Octane_Battle_Ranking'      (FFA scoreboard)
                 : 'Octane_BattleTeam_Ranking'  (TEAM scoreboard, winner banner)
```
During race the same mask opens `'Octane_Race_Leaders'`; `'Octane_Minimap'`
opens for 1..3 local players when mode not in 9..13.

For team SingleRace/Cup the native switch still opens Race/Cup ranking because
the race mode remains 1/6. Do not route those modes through Battle ranking
without a separate design requirement; the Battle result chooser already
selects `Octane_BattleTeam_Ranking` for Battle modes 9–13 when `teamCount >= 2`.

## 3. Private match data + sync (host → guests)

Class layout (decompiled ctors):

| class | size | fields |
|---|---|---|
| `COctaneLobbyPrivateMatchData` | 0x28 | `_mapName` (string), `_useAI`, `_difficulty` |
| `COctaneLobbyPrivateMatchRaceData` | extends | `_lapCount` (int) |
| `COctaneLobbyPrivateMatchBattleData` | 0x108 | battle rows (points/time/lives; teamCount lives here for battle)-type fields |
| `COctaneLobbySyncPrivateMatchDataEvent` | 0x40 | syncs the bundle; field accessors include `_raceMode` (`EOctaneRaceModes`) and `_gameType` (`EAsyncMatchmakingGameTypes` = {eAMGT_Invalid, eAMGT_Race, eAMGT_Battle}) |

Flow on the private lobby VSC graph (`Octane_Lobby_PrivateGame_Root.igz`,
`Octane_Lobby_PrivateGameSettings_Root.igz`):
`broadcastSettings_VSC` → send `COctaneLobbySyncPrivateMatchDataEvent` → menu side
fires `CGuiEventPrivateMatchDataChangedEvent`; `saveCurrentRaceSettings_VSC`
pushes values into the race-settings singleton. All these VSC nodes are native
C code invoked from the graphs, so asset-level un-hiding gives net-synced team
settings without writing new sync code.

## 4. Team select screen (native, already complete)

- Asset `ui/Octane_TeamSelect.igz` + VSC graphs `Octane_TeamSelect_Root/Close/TeamLabel`
  + `octane_teamselect_en.lng` in `juicedomain_onlinemenus.pak` (both regions):
  4 player slots (`CGuiBehaviorOctaneTeamSelectPlayerSlot`), animation tags
  `TwoTeams/ThreeTeams/FourTeams`, localized team names (Red/Green/Yellow/Blue),
  warning "All teams must have at least one player!".
- Classes: `CGuiBehaviorOctaneTeamSelect`, events `COctaneTeamSelectRequestTeamEvent`
  / `...ConfirmTeamEvent` / `...UnconfirmTeamEvent`
  / `CGuiEventTeamSelectPlayerReady` / `CGuiEventTeamSelectPlayerChangeTeam`
  / `CGuiEventConfigureTeamSelect` / `CGuiEventCloseTeamSelect`.
- Lobby wiring already present in the shipped asset:
  `Octane_Lobby_PrivateGame_Root.igz` calls `openByName("Octane_TeamSelect")` and
  listens for `CGuiEventEnterTeamSelectFromLobby`; the USA event factory is
  `FUN_01afd116` (EU `FUN_01afd0f6`). The VSC listener handles navigation to
  the project; the factory is registration, not itself proof the screen opens.

### 4.1 Battle team flow, from lobby setting to player choice

This is the native flow reconstructed from the 1.21 USA executable and the
extracted `juicedomain_onlinemenus.pak` / `update.pak` assets. Team count,
opening the Team Select screen, and assigning a player's team are separate
steps.

1. **Host chooses a Battle team count.** The private-game settings data has a
   Battle payload (`COctaneLobbyPrivateMatchBattleData`) and the lobby sends it
   with `COctaneLobbySyncPrivateMatchDataEvent`. The team-count selector is a
   native `CGuiBehaviorTeamCountSettingSelector`. The global race setting at
   `0x02ad4e30 + 0x28` is `teamCount`: `1` means no teams/FFA; `2`–`4` select
   that many teams. Showing the settings row does not itself assign players.
2. **The setting establishes initial assignments.** `FUN_00bf32d0(settings,
   count)` stores `count` and distributes the player list. Its local path assigns
   player `i` to `i % count`; the online-host path delegates to
   `FUN_00bf3390`, which balances players across the selected teams.
   `FUN_00bf36c0(player, teamIndex)` writes the player's team index and takes
   the network event path for network players. Changing team count can therefore
   alter initial assignments without opening Team Select.
3. **Team Select is a separate lobby screen.** The lobby root listens for
   `CGuiEventEnterTeamSelectFromLobby`; its VSC opens the project
   `Octane_TeamSelect`. The screen has four player-slot components and layouts
   `TwoTeams`, `ThreeTeams`, and `FourTeams`. The team-label graph listens to
   `CGuiEventTeamSelectConfigureTeam::_teamIndex` and displays localized team
   names. Verified index order: `0=Blue`, `1=Red`, `2=Green`, `3=Yellow` (index
   0 is also shown as BLUE TEAM in the supplied screenshot).
4. **Changing/locking a team uses native request and confirm events.** The
   executable registers `COctaneTeamSelectRequestTeamEvent`,
   `COctaneTeamSelectConfirmTeamEvent`, and
   `COctaneTeamSelectUnconfirmTeamEvent`; the UI also has
   `CGuiEventTeamSelectPlayerChangeTeam` and
   `CGuiEventTeamSelectPlayerReady`. This proves support for changing a slot's
   team and confirming/unconfirming it, including network requests. The root
   listens to `igGuiEventProjectInputPressed` with signal filter values `2`,
   `3`, `6`, and `7`; its button legend includes enum values `1` and `2`.

   **Physical button mapping remains unverified.** The executable registers
   `EButtonLegendButton` with 13 names, but the mapping from enum values and
   input-signal ordinals to controller labels/actions has not been resolved.
   So this evidence does not justify claiming “press A to ready, left/right to
   change team, B to back.” The native events show the available actions; the
   exact controls/glyphs need the input enum or runtime UI confirmation.
5. **Closing uses the native menu flow.** `Octane_TeamSelect_Close` listens for
   `CGuiEventCloseTeamSelect` and carries `_previousMenuName`, along with native
   close/open and dialog nodes. The assets support returning to the previous
   menu, while the physical back-button mapping remains part of the unresolved
   input mapping above.

### 4.2 What an assigned team looks like

- **In the Team Select lobby screen:** the configured team index produces a
  localized Blue, Red, Green, or Yellow label for the player slot. The assets
  include “Player %d”, “Ready”, and “All teams must have at least one player!”
  and provide layouts for two, three, or four teams. The expected presentation
  is the dedicated Team Select project with team groups and ready state, not a
  Team Select overlay obscuring the lobby's Start Game panel.
- **During a race:** the replicated player `_teamIndex` feeds
  `CTeamFactionReplicaManager` (`_teamPlayers`) and entity team/faction data
  (`EEntityTeam`, `EEntityTeamFaction`). `CTeamFactionSwitchNode2` exposes
  faction branches to gameplay graphs, so team-dependent entity visuals can be
  selected. Battle Arena also has a dedicated team-score HUD component
  (`CGuiBehaviorBattleArenaScore`) and red/blue score placeables. This proves
  team identity and team score are represented in-race.
- **Limit of current evidence:** these classes/assets prove faction-driven
  visual switching and the team-score HUD, but do not prove every kart body is
  recolored, the player's name changes color, or the exact decal/tint per
  faction. Those details require tracing the Battle gameplay/entity graphs;
  a lobby label alone is not proof that the racer appearance is correct.

### 4.3 Implications for the observed lobby bug

Expected order: set and sync team count → native initial assignment → enter
`Octane_TeamSelect` through the lobby event → players request a team and confirm
readiness → race setup consumes replicated team indices. At `teamCount=1`
there should be no team choice (FFA); at `2`–`4`, the matching layout and team
labels should be used. A selector that appears on top of the lobby, covers
Start Game, or starts a race when backing out is inconsistent with the native
separate-project navigation. Asset evidence alone does not prove the exact
condition that makes the lobby emit `CGuiEventEnterTeamSelectFromLobby`; that
gate still needs runtime tracing.

## 5. teamIndex → racer faction (why teams actually appear on track)

- Player-side: `netSetTeamIndex / netGetTeamIndex` (`_teamIndex`),
  net event `CPlayerSetArenaTeamIndexEvent` replicates it.
- Race-side replica: `CTeamFactionReplicaManager` replicates `_teamPlayers`
  (array of player handles' team).
- Entity-side: entity data carries `EEntityTeam _team` (4 values) and
  `EEntityTeamFaction _teamFaction` (12 factions `eETF_Faction_1..12`);
  VSC node `CTeamFactionSwitchNode2` switches `_choice` among
  `_none/_faction1/_faction2/_outOfRange` inside gameplay graphs, so visuals
  (decals/colors) come from GUI data keyed by faction.
- Rule numbers: `team_balance` ruleset payload with `num_teams`
  (`FUN_00403cb0`) and `can_change_teams` (`FUN_00400380`) — DB-driven config
  from the online ruleset, not the matchmaker.

## 6. Score during race + team winner

- In-race team score HUD panel class: `CGuiBehaviorBattleArenaScore` (0x70,
  referenced from racer-HUD assets; `_teamRedScorePlaceable/_teamBlueScorePlaceable`
  strings in `Octane_Racer_Hud_BattleMode` assets).
- Team standings model: `CMultiplayerTeamStatistics`, meta-sorters
  `CBattleScoreSorterMetaField` / `CTeamBattleScoreSorterMetaField`
  (`_playersBattleScore`, `_teamBattleScore`).
- End-screen project `ui/octane_battleteam_ranking.igz` (update2.pak):
  `BattleScoreText`, `WinnerTitle`, `[WinnerNameText]`, `RankingList`,
  behaviors `CGuiBehaviorOctaneRaceRankingEventCatcher` /
  `CGuiListItemPopulatorOctaneRaceRanking`; its root VSC graph exists too
  (`vsc/ui/octane_battleteam_ranking/octane_battleteam_ranking_root.igz`).

## 7. Private settings menu asset (update.pak)

`ui/octane_lobby_privategamesettings.igz` USA hash
`06a0b888452e8745d93ef12f3cabe063ef791297f9985c12106edf5621ceadea` already
contains: `CGuiBehaviorTeamCountSettingSelector` (+`_maxTeamCount`),
`CGuiBehaviorBattleTypeSettingSelector`, `CGuiBehaviorRaceLapSelector`,
`CGuiBehaviorTimeLimitSettingSelector`, `CGuiBehaviorPointLimitSettingSelector`,
`CGuiBehaviorWinningConditionSettingSelector`, `CGuiBehaviorNumberLivesSettingSelector`
and animation tags `HideTeams/HideType/HideDifficulty/HideLapCount/HideLives...`
played for race (non-battle) modes. Un-hiding the Teams (and Battle Type) rows
for race modes is an IGZ/VSC patch in `scripts/online_mode.py` style.

## 8. Implementation status and next steps

The implementation must preserve the stock Battle flow and patch only private
lobby UI/navigation where the project lacks an entry point:

1. **Keep host settings authoritative.** The existing private settings sync
   (`broadcastSettings_VSC` / `COctaneLobbySyncPrivateMatchDataEvent`) and the
   native TeamCount selector should carry the selected count to every peer.
   Do not force `teamCount >= 2`: `1` is the user's intentional FFA choice. The
   current C helper reads the value for logging and button visibility; there
   is no `force_private_team_count()` implementation.
2. **Add a separate Team Select action below Friends.** Keep the Friends row
   and its `common_octane_gui_open_friends` action intact. The private-game UI's
   actual `Buttons` container is rooted at placeable `0xba0` with child table
   `0xcc0`; its five native rows are `0xd20`, `0xe40`, `0xf60`, `0x1080`, and
   `0x11a0`. The Friends row is `0xd20`, with navigation behavior `0x3368` and
   Friends VSC behavior `0x35d8`. Clone that complete button subgraph, add the
   new row to the container and focus chain, and dispatch the existing
   `CGuiEventEnterTeamSelectFromLobby` event. The lobby root's listener is at
   `0x6978` in `octane_lobby_privategame_root.igz` and opens
   `Octane_TeamSelect`; the native selector still owns assignment and ready
   state.
3. **Keep assignments peer-visible.** Team changes must use the native request,
   confirm, and unconfirm events. Verify each accepted change updates
   `_teamIndex` through the native network event before race setup; do not add a
   local-only toggle.
4. **Keep Battle presentation native.** The in-race faction/HUD and Battle team
   result project are already in the game. Runtime must not stack a second
   result project over the native flow. The duplicate
   `Octane_BattleTeam_Ranking` open previously added to `race_ui` was removed;
   the native chooser/result event owns this screen.

**Still to verify in-game:** host/guest confirmation that team changes
replicate, Battle start with AI, and exact in-race kart/faction visuals. The
Team Select action is now implemented as a separate row (see §14), but its live
open/close behavior still needs verification. A prior clone mistakenly
copied placeable `0x4030`, which is the map preview frame, and wrote navigation
references outside that object's `0x120`-byte size. The resulting custom handle
`0x8000001b` was the fault address in the 2026-11-01 lobby crash log. That
unsafe row clone was removed before the replacement in §14. Do not clone
`0x4030` or relabel Friends. Signal ordinals are known,
but controller glyph mapping is not (see §4.1).

## 9. End-of-race flow, winner state, and podium coupling  (VERIFIED in both dumps)

All of this was extracted from the race-end listener fan-out; equal-address pairs are in
§10. The flow is registered by the race-end listener **`FUN_00e9b180`** (EU) /
**`FUN_00e9b190`** (USA) — the race-object's slot `0x...a0` (`+0xb0/0xb8/0xb8` fans: it
hooks `param_1+0x60..0x88` handler slots); the master dispatcher is
**`FUN_00ea4530`** (EU) / **`FUN_00ea4540`** (USA) (+0x10) and reaches the ranking screens
by opening projects (`open_project 0x986820` at `FUN_00e9ba20/30`):

- `FUN_00ea4530`: handles all end-of-race buckets, but the **one and only class**
  that produces the team-score event is the battle arena component; its
  "score" handoff is the single sender **`FUN_00eab2f0`** (EU) / **`FUN_00eab300`**
  (USA). The send helper is **`FUN_01209490`** / +0x30: it registers the
  event class meta **`CGuiEventBattleArenaResult`** at the (identical-address)
  singleton **`0x02ae2608`** (CTR_EU/CTR_USA decompiles both list
  `FUN_01aed360`/`01aed418` + `DAT_02ae2608 ... "CGuiEventBattleArenaResult",0x28`
  — 0x28 bytes, metafield `_isWinnerName` + `_resultString` as the VSC-side names
  used by `octane_battleteam_ranking.igz`).
- **Winner banner data** (from `FUN_00eab2f0` pseudocode, verified side-by-side
  EU and USA): the bucket checks the *team route* branch **only** when
  `race_settings.teamCount (DAT_02ad4e30+0x28) >= 2`, and the winner byte is an
  aggregation of the racers' finish state — `uVar13 = *(uint *)(playerEnt + 0x78)`
  is a **flag bitmask triaged bit-by-bit (0x16..0x1f)** into a small
  placement index `iVar12` (0x16..0x21); `local_34` counts entries and
  `*(char *)(localEvent + 0x18)` is set to **1 only while every processed
  racer shares the same placement** (`iVar16==iVar12`; first racer sets it,
  any mismatch clears it) — that byte is the `_isWinnerName` input. The —
  **`iVar15 == 2 && iVar2 == 9`** check (`settings+0x2c == 2` + `mode == 9`)
  is the only podium/score discrimination inside `FUN_00eab2f0`: mode-9 with
  a *podiums* value (via the `race_manager+0xd4c` chain +0x50/iVar15 above)
  makes even a **local_34==0** (no placed racers) run take the
  `DAT_02ac0dc0+0x28+0x3e0` (podium) branch rather than the +0x3d8 (score).
- **Podium-id / criterion**: the "battle vs podium" fork is exactly one
  branch: `if (local_34 == 0 && (mode == 0xb || (settings+0x2c == 2 && mode == 9)))`
  → podium connector `0x3e0`; **everything else** → score connector `0x3d8`.
  The `+0xd4c` value being one of the battle modes (9..13) does **not** by
  itself force the podium here — **`FUN_00eab2f0`'s own battle check wins**:
  the branch-point uses ONLY `iVar2 (= +0xd4c)` `== 0xb` (`StealTheBacon`)
  and the pair above. **Consequence for the hookup:** to feed the winner
  banner from a NORMAL/ITEMLESS race, the flow needs teamCount >= 2 in
  `DAT_02ad4e30+0x28` (the same teamCount the results picker reads —
  verified §2) — **the banner event then fires by itself**.

**Conclusion for our hook (objective c from the user):** `FUN_00eab2f0/0eab300`
already posts the winner event when, and only when, the racers share one
placement; the **Octane_BattleTeam_Ranking** project picks up `_isWinnerName` and
`_resultString` itself (from its VSC on-field `_isWinnerName`/`_resultString`,
§6) and clears `race_manager+0xd90` by itself. **No code, no data shape, no
rewiring — only the routing has to reach it.**

## 10. Address map (region pairs)

| function | EU | USA | notes |
|---|---|---|---|
| results picker | 0x00e9ba20 | 0x00e9ba30 | +0x10 |
| arena gate | 0x00ee7c90 | 0x00ee7ca0 | +0x10 (`Requires Race Mode to be an Arena type.`) |
| sports gate | 0x00e81760 | 0x00e81770 | +0x10 |
| arena setup | 0x00de8e30 | 0x00de8e30 | identical |
| teamCount setter | 0x00bf32d0 | verify (~+0x10) | shares `open_project`/globals |
| open_project | 0x00986820 | 0x00986820 | identical |
| register event helper | 0x01209490 | 0x012094c0 | +0x30, only caller 00eab2f0/00eab300 |
| race-end master | 0x00ea4530 | 0x00ea4540 | +0x10, hooked by 00e9b180/00e9b190 |
| score-event sender | 0x00eab2f0 | 0x00eab300 | master's +0x68-bucket handler |
| CGuiEventBattleArenaResult | 0x02ae2608 | 0x02ae2608 | class meta global, same addr |
| set_race_mode | — | 0x00fdeb70 | repo-known |
| PrivateMatchData reg | 0x019971a6 | 0x019971c6 | +0x20 |
| SyncPrivateMatchDataEvent reg | 0x01afaad8 | 0x01afaaf8 | +0x20 |
| EnterTeamSelect factory | 0x01afd0f6 | 0x01afd116 | +0x20 |
| GUI event regs (team select) | 0x018ae9ec.. | +0x10..+0x20 | validated in both reports |
| dev/console funcs | equal | equal | build noise |

Globals `0x02acde48/0x02ad4e30/0x02ac0dc0/0x02acb3d8/0x02ad1160/0x986820` are
byte-shared between the two dumps (see decompiled bodies in
`/tmp/opencode/ctr_re/{eu,usa}/ctr_xrefs_eboot.elf.md`).

## 11. Open items

- Exact BattleData field offsets (points/lives/time; teamCount row id) — confirm
  by dumping the applied GUI field list before writing item remaps.
- Whether `EEntityTeam` 4-slot model suffices for "4v4" (it does: 2 teams) and
  how far `_teamFaction` (12 slots) maps to the TeamSelect color palette
  (Red/Green/Yellow/Blue) — visual only, no engine risk.
- In-race team HUD for race modes: choose between `Octane_Race_Leaders`
  (already shown for our mask) and embedding `CGuiBehaviorBattleArenaScore`.

## 12. Online menu regression (2026-10-31)

The experimental `add_team()` clone in the shared ModeSelect asset was removed,
following the runtime-only approach in §8. On the USA 1.21 asset it replaced the
child array with `[MODE, TEAM, button]`, while TEAM's next-sibling pointer still
referenced RACE TYPE. It also reloaded RHND/RNEX from the original asset, losing
the relocations added for RACE TYPE. The resulting graph was inconsistent before
the player entered a private lobby; the reported symptom was an empty Online panel.

The selector regression test now checks that child-array order and sibling links
agree. All ten `test_online_mode.py` tests pass with the local USA 1.21 dump after
removing the clone. The existing private TEAM runtime hooks remain unchanged;
this validates the menu asset structure, not team selection, network sync or the
winner screen in a live match.

## 13. Lobby Team Select entry (2026-10-31)

The attempted Friends-row replacement was reverted after the 20:58:48 runtime
log showed a SIGSEGV while the game's native VSC code built the replacement
graph (guest offset `0x25f03d`, dereference at address `0xa8`). Vulkan had
initialized successfully, so this failure was in the menu graph path, not GPU
enumeration. Do not claim that the Friends row is a working Team Select button.

The native team-count setter was separately verified in the USA 1.21 image at
`0xbf32d0`: it writes `teamCount`, assigns each online player through the host
path, and stores the selected team index at player offset `+0xe4`. The host's
setting is therefore already the authoritative assignment input.

An earlier attempt used the lobby Friends action's shipped `openByName` graph, retargeted to
`Octane_TeamSelect`. This graph is duplicated in `update.pak` and
`juicedomain_onlinemenus.pak`; the lobby label is duplicated across those and
`update2.pak`. All copies must be patched together. A prior direct `open_project`
shortcut showed the screen as a small lower-left overlay without focus, so it
was removed. The native graph owns placement, transitions, focus, and return.

The latest log showed team counts syncing, but only the archive table record
for the replacement graph was read; the game never read its replacement data.
That exposed the unpatched duplicate as why the lobby still acted like Friends.
Live two-client confirmation of the corrected route and synchronized display is
still required. That Friends replacement is no longer the implementation;
the independent action below preserves Friends.

## 14. Separate Select Team button (2026-11-01)

`add_team_button()` copies the Friends placeable `[0xd20, 0xe40)` and its
complete behavior/animation block `[0x3330, 0x3a50)`. ROFS references within
those blocks are relocated; RVTB, RSTT, RHND and RNEX sites are copied and
serialized through `Igz.sites`. The shared native button asset handle is
reused, avoiding the unresolved custom component handle from the earlier crash.
The new row is attached to the actual Buttons container and traversal chain.
Its initial position and all four host/peer animation positions are one button
pitch (12, 105) below Friends.

The cloned VSC is a separate `Octane_Lobby_PrivateGame_TeamButton` class,
included in the online streaming package and loose-file map. It preserves the
Friends graph's native focused-confirm input filter and root sender handle,
but emits `CGuiEventEnterTeamSelectFromLobby` instead of the Friends graph
event. The original Friends VSC and graph remain unchanged. The shipped lobby
root handles the event; no direct `open_project` call or start-game action is
added by this patch.

`update_team_button()` reads the current native `teamCount` on lobby refresh.
Counts 2, 3 and 4 reveal SELECT TEAM and insert it between Friends and the
Start/Ready wrap target. No Teams (1), zero and invalid counts hide it and
bypass it in both navigation directions. It starts hidden in the asset.
The runtime uses the current local native settings on both host and guest;
this patch does not introduce a separate team-count source or replication path.

Validation: 13 Python tests pass against the USA 1.21 assets, including clone
relocations, unchanged Friends behavior and the distinct event binding. The C
runtime test passes for both regional hook layouts and exercises transitions
1 → 2 → 3 → 4 → 1 → 4 → 0 → 5, checking visibility and both focus links.
Full `prepare()` succeeds with the generated graph present in the file map;
Linux rebuild succeeds. Live appearance, team selection/Back, host/guest sync
and Windows compilation have not been verified in this change. Automated GUI
inspection is unavailable because Orca's Python cannot load GI/AT-SPI.

### 14.1 VSC name-hash regression

The `20261101_021809.log` crash at guest `0x25f03d`, address `0xa8`, occurred
during graph construction. That instruction writes a callback at `meta+0xa8`
after querying the behavior's type; the returned meta pointer was null.
The generated TeamButton graph renamed its exported strings but retained the
FriendsButton hashes in `igNameList`: `0xb455c656` instead of `0xbc7a2adc`
for the class, and `0xfc4f76c6` instead of `0x11183ba4` for its delegators.
The class therefore could not be found under its new exported name.

`team_button_graph()` now recalculates each serialized name hash with the
existing case-insensitive FNV-1a helper. The regression test follows the name
hash to the corresponding root-list object and checks that it is the renamed
`igVscMetaObject`; checking strings and relocations alone missed this bug.
All 13 asset tests and full launch preparation pass. Live confirmation after
this correction is still needed. This change is Python-generated asset data;
`run.sh` regenerates it even with `BB_PREBUILT=1`, so no executable rebuild is
required for this correction.

## 15. Removal of the rejected button (2026-11-01)

Removed the button clone, neutral button container, custom TeamButton VSC,
streaming entry, runtime visibility/navigation logic, and experimental lobby
control-route clone. Keep the native Teams settings visibility patch. Preparation
removes the stale generated TeamButton file before rebuilding its loose-file map.
The previously added explicit RVTB registration was redundant: the relocation
loop already copied that site. It was not a demonstrated crash fix.

Latest crash: `20261101_040411.log`, guest `0x14a94e9`; instruction
`cmp %rbx,0x50(%rcx)` during linked-pointer traversal. This identifies an invalid
pointer traversal, not a proven allocation responsible for the corruption.

USA executable tracing: the sole direct caller of the EnterTeamSelect event
factory wrapper `0x120f1a0` is `0xebabf8`, inside function `0xebaaf0`.
That function obtains the lobby project using race manager `+0xdf0`, builds
and dispatches the native event. Its callers appear indirect; the state-machine
entry condition still needs tracing. The shipped ButtonReady graph calls
`CNetworkNodes::lobbyStartGame_VSC` and must remain native.

Removal checks: 12 Python asset tests pass against USA 1.21; focused C runtime
test passes. Rebuilt only out/sacredfire against the existing GPU library.
No live two-client verification or team scoring implementation yet.

## 16. Native countdown gate (in progress)

Verified USA `0xeb7950` / EU `0xeb7940`: the lobby timer compares remaining
time to `self+0x2c`, then checks race mode 9..13 and teamCount >= 2. It creates
`COctaneLobbyEnterTeamSelectEvent` at USA `0xeb7a5a` and sends it via native
network dispatch `0xa90530`; the handler `0xebaaf0` dispatches the GUI event.
The gate is USA `0xeb79d3`, EU `0xeb79c3` (21-byte signature checked).

Implemented a trampoline extending this gate to mode 1 only when the native
private-lobby query is true. The race mode is never written. Battle keeps its
native gate and teamCount check. No new button or direct project opening.
The trampoline preserves caller registers and FP/SSE state. Executable C tests
exercise all modes 0..15 in public/private for both regional layouts and assert
that the actual race mode is unchanged. Tests pass after correcting a test-only
scratch-address collision with the existing podium stub.

Still outstanding: other team-mode gates, team-count synchronization for race
settings, racer team assignment (including AI), 9/6/3/1 team totals/presentation,
and live two-client countdown/race verification. Do not treat this gate alone
as completion of team racing. GPU has not been rebuilt.

### 16.1 Settings synchronization findings

The native race-settings event builder `0xc2eae0` copies settings `+0x28..0x37`
(including teamCount) to event `+0x30..0x3f` without a race-mode gate.
`0xc2ed20` broadcasts this event; `0xc2ec90` targets a peer. However,
private lobby settings-change handler `0xebc8a0` calls the broadcast only if
network `+0x13c == 2` (Battle). The private-match UI event alone does not carry
teamCount: it carries type/map/useAI/difficulty/laps/mode. The prior blanket
claim that unhiding the row automatically guarantees synchronization for racing
was therefore not justified. This additional gate needs an explicit correction.

User reiterated: both clients must enter Team Select together and see each
other's team changes. Preserve the native request/confirm network route; local
UI state is not acceptance evidence. The saved Ghidra projects at
`/home/n0bu/ghidra_ctr/proj/CTR_USA` and `CTR_EU` are available read-only.
JDK runtime has compiler module but no javac launcher; `/tmp/ctr-team-jdk`
provides a temporary launcher for the existing module, without a system install.

### 16.2 Implementation in progress: settings and results

`private_settings_sending` now invokes native `0xc2ed20` for private Single Race
payloads (type 1/mode 1), including No Teams. Existing sender argument preservation
is reused. A focused test checks counts 1..4 and that public settings do not
invoke this added broadcast. Native broadcast entry is identical in USA/EU.

Added results calculation using host event IDs/order (the existing CUP receiver
hook). Recomputes 9/6/3/1 each packet, validates unique IDs/ranks and team bounds,
and commits totals only after all racers validate. Human team uses player+0xe4;
AI faction uses native `0xff1810` (EU -0x10) minus 0x16. Ghidra getter
`0xc37080` confirms faction bits 22..33 map to team indices 0..11.
The native Single ranking handle is `race_ui_owner+0x60`; capture only for
private mode1/teamCount2..4. Existing row labels `[PositionText]`/`[Timer]`
are used to display `BLUE +9 | TEAM 12` etc after final results; native finish
positions/timing data are untouched. No new GUI object graph is constructed.
Results and pinned project are reset on race UI creation and map load.
This UI replaces the displayed timer text in final team results; individual
native time data remains present. Visual fit has not yet been checked live.

## 17. Current implementation and verification (2026-11-01)

Three Battle-only paths now support private mode 1 (Normal/Itemless): the
native lobby countdown gate, race-settings snapshot broadcast, and AI faction
assignment. The real race mode remains SingleRace. The separate SELECT TEAM
button and experimental object graph are removed; Start retains the native
network event that opens Team Select on host and guest.

Snapshot receiver `0xbf3110` applies the event mode and calls team-count setter
`0xbf32d0` without a Battle-only gate. Player team changes retain native
`0xbf36c0` / `0xc373d0` replication and `0xc36e10` entity application; no
second local-only team state or alternative network protocol was added.
Actual two-client ordering, countdown display, and team-change visibility
still need a live test; static call tracing is not proof of synchronization.

Correction to the initial AI inference: helper `0xe9dcf0` itself accepts any
mode, but its caller `0xe9d3a0` invokes it only for Battle with an arena entity.
The new `0xe9de80` hook invokes that helper at the same setup stage for private
SingleRace with 2..4 teams and AI enabled. The helper enforces host authority,
balances AI against populated factions, and applies native entity factions.
The hook preserves the existing race setup. Both addresses shift -0x10 on EU;
the stolen 17-byte setup prologue is signature checked before patching.

Scoring uses the host finish order and validates a complete mapping to racers
before committing totals. Tests cover two/four teams, native AI faction IDs,
duplicate packets (no accumulation), duplicate finish IDs, invalid teams,
and public-mode exclusion. Existing native race rows display individual award
and team total; this does not reuse Battle scoring or its winner banner.
Earlier claims that teamCount alone would supply a SingleRace team winner
banner are not established. Label fit and refresh during real GUI animation
remain live-test items.

Validation: focused C runtime suite passes for both region layouts, including
executable countdown trampolines for modes 0..15 and the AI setup hook across
public/private, modes 1..13, team counts 1..4, AI on/off. A test-only return
stub initially omitted its final RET byte; corrected the fixture and reran
successfully. Python asset/signature suite passes 12 tests against USA and
12 against EU 1.21, including new native function signatures. GPU build is
explicitly excluded; only the game executable is rebuilt against the existing
library. `run.sh` regenerates online assets even with `BB_PREBUILT=1`, including
removal of a stale generated TeamButton asset.

Executable build succeeded: `out/sacredfire`, 2,045,016 bytes, timestamp
2026-11-01 04:27:28 +0700. GPU library timestamp remains
2026-10-31 20:25:38 +0700. `git diff --check` passes. The pre-existing dirty
GPU submodule was not modified by this work.

Live acceptance still required on both updated peers: choose 2 teams, Start,
observe simultaneous native selector/countdown, move each player between teams
and observe both screens, finish Normal and Itemless with AI on/off, verify
summed 9/6/3/1, then repeat No Teams and Battle for regression coverage. The
original crash's precise invalid-pointer origin has not been proven; removal
of the added GUI graph must not be described as a verified crash cure.

## 18. Feedback from 04:29 session: controls, Itemless, results

User confirmed Team Selector opens, but cannot change teams. Also reported item
boxes despite choosing Itemless, absent team indicators/scores, and requested
the native Battle team victory presentation rather than a newly invented
three-character podium format.

Latest play log: `~/.local/share/sacredfire/logs/20261101_042946.log`.
Line 1788 records `Native Online mode: ITEMLESS`; line 1800 records successful
room advertisement as ITEMLESS (RyuNET 200). Thus the UI selection was Itemless,
not Normal. Overlay preparation was logged at startup, but that alone does not
prove private overlays were active when the track loaded. No team-result log
appears in this session. The log ends with a normal window close.

### 18.1 Native Team Selector starts online players ready

Verified initialization `0x9c13b0`: when the lobby component exists, local bool
`[rbp-0x31]` is set to 1, then inserted into the player-ready table at `+0x60`.
Movement `0x9c4780` immediately returns for a ready player. This is independent
of the request/confirmation network functions: the input cannot reach them.
`0x9c22a0` routes directional signals 2/3/6/7 to that movement function.

The new trampoline replaces only initialization's table-insert call at
`0x9c153f` (same address both regions, regional displacement signature checked).
For private mode 1 it supplies false to the original native table insertion;
Battle/public retain the original value. It restores the stolen frame-local
loads before resuming. Native movement, capacity checks, ready/unready actions,
host request validation, player-team replication, and countdown remain native.
Tests cover public/private across modes 0..15 and ensure the input bool is not
mutated. Actual host/guest movement still needs a live test.

### 18.2 Preserve private race eligibility across lobby teardown

`0xf50350` resolves the lobby component and reads its private byte `+0x44`.
It returns false when that component cannot be resolved on a race map. The
previous Itemless predicate and team UI/AI setup queried it during the race.
That was an invalid lifetime assumption, also exposing the team features to
being disabled after the lobby was torn down.

Private SingleRace eligibility is now captured at the map-load boundary and
retained for the track. It clears on a later map load or room departure; lobby
and start-screen maps do not acquire it. Existing authenticated session/recent
peer checks in the Itemless predicate are retained. Tests simulate losing the
lobby component on the track and returning to the lobby. Added load diagnostics
show map, active NORMAL/ITEMLESS predicate, and team count. The old log did not
record those predicate values, so this is a corrected code defect, not proof
that it was the sole source of the reported item boxes.

### 18.3 Native BattleTeam victory screen and visible scores

While native race UI is being constructed, an OpenProject adapter substitutes
`Octane_BattleTeam_Ranking` for `Octane_Race_Ranking` only in private mode 1 with
2..4 teams. It occupies the original ranking slot; no second result screen is
stacked, and the real mode remains SingleRace. The factory hook has a verified
17-byte prologue and forwards all arguments, including delay and player slot.

Battle's result presenter `0xeab300` selects racers with result position
`+0x64 == 1`, resolves their faction, and feeds a winner/draw string to the
BattleTeam project. Correction to section 9: GUI manager `+0x3d8/+0x3e0` in
that function are **string handles**, not proof of a cinematic podium branch.
The verified reusable presentation is the native team victory/ranking screen.
For SingleRace the adapter derives the winner/draw from summed CUP points.
The individual online race podium pending flag is cleared on returning to the
lobby after this team screen, preventing an individual winner ceremony from
contradicting the team result. No new 3D four-character podium was implemented.

Native list population `0x9b6180` still uses SingleRace's per-racer order. Final
rows retain that order, prefix player names with `[BLUE]`/etc, and display team
total in the existing `[BattleScore]` field. The existing `[AliveTime]` field
receives individual `+9/+6/+3/+1` where that field is visible. WinnerName/Wins
labels show the highest team total, or a draw. Duplicate packets recompute.

Another corrected defect: the prior text visitor did not enter the native
`igGuiComposition` wrapped by each project. Both result and leaderboard visitors
now traverse the verified composition's root, as the existing lobby code does.
Tests exercise this real wrapper shape, rather than only a flat mock GUI.

The native Race Leaders project in race UI slot `+0x58` now shows compact team
indicators alongside position: e.g. `1 B12` means first position, Blue, 12 team
points (B/R/G/Y = Blue/Red/Green/Yellow). Live racer position is `+0x58`, not
the finish position `+0x60`. Before the final host packet, points reflect racers
marked finished locally; the complete host packet replaces them authoritatively.
Refresh runs after native project events and racer-finish callbacks. No claim is
made yet about live label fit or every HUD refresh cadence.

Validation: runtime tests pass for both regional layouts, covering unlocked
initial readiness, retained private eligibility, BattleTeam project routing,
composition-wrapped results, winner label, name prefixes without accumulation,
AI totals, and HUD totals 0→9→12. Asset/signature tests pass 12/12 for USA and
12/12 for EU. Remaining live checks: host/guest controls and synchronization,
Itemless pickup removal after load, native BattleTeam animations/list visibility,
HUD fit/updates, and return-to-lobby behavior. GPU is not rebuilt.

Build succeeded at 2026-11-01 04:47:36 +0700: `out/sacredfire` is 2,061,168
bytes. Existing GPU library still has timestamp 2026-10-31 20:25:38 +0700.


## 19. Correct the observed eight-player results and disabled controls (2026-11-01)

Live evidence: `20261101_044924.log` loaded Crash Cove with `mode=ITEMLESS
teams=2`; final host totals were Blue 6, Red 13. The screenshot's repeated
Blue/Red names do NOT prove those were the eight actual team assignments.
Section 18's presentation and podium claims are superseded below.

The BattleTeam asset serializes `CGuiBehaviorOctaneRaceRankingSelectItem +d8`
as true. Native event handler `9b73f0` preserves this flag when its configured
row data `+86` is false (the race populator's normal path). As a result native
`9ba1f0/9b8400/9b94e0` resolve a TEAM statistics row rather than the individual
racer. A visitor parsing the resulting position text cannot fix this.

New adapter hooks event slot `25436c8`, clears d8 BEFORE native binding only
for the private team race project, and preserves native player/AI names.
After native processing it resolves the bound racer handle at +78, matches
human/id/online-id against the complete validated host snapshot, and writes
finish position to +b8 and individual CUP award to +d0. All eight finishers
remain distinct; awards are 9/6/3/1/0/0/0/0. Team totals appear in the winner
header, not repeated in each player's point column. Project refresh walks
actual row behavior lists instead of inferring identity from display text.
Malformed/duplicate identities reject the entire new snapshot; duplicate
valid deliveries recompute, never accumulate. Tests exercise all eight bound
identities in reverse GUI order and malformed packets.

The rejected B0/R0 position text is removed. RaceLeadersItem event/update
slots 2546008/2546018 resolve native racer handle +88 and color ONLY the
portrait contour placeable +50. This is not +40 (the flag sprite) nor the
portrait image. Float RGBA at placeable +c0 and dirty bit 1000 match native
252c10. A Yellow AI border is covered by the adapter test.

The lobby's disableControls node calls a09b80: it disables the project AND
sets GUI manager input +e0 -> +18 false. enableControls calls a09190, which
restores both. After opening Octane_TeamSelect in private next-mode 1, call
that native enable helper on the returned project. Initial ready=false remains;
no raw controller polling or local-only team assignment was introduced.
Public and Battle selector opens are left to native behavior. Tests cover this
scope; actual host/guest input and network movement still require live testing.

Removed the incorrect team-return clearing of online podium pending +71.
BattleTeam ranking is NOT a substitute for a cinematic podium. This restores
the native ceremony eligibility; team podium participant selection remains
under investigation. Overhead AI names/team colors also remain in progress.

At this checkpoint runtime adapter tests pass for both layouts and 12/12
asset/signature tests pass per region. Newly added UI vtable addresses match
both USA and EU dumps. No GPU build performed. These tests do not substitute
for two running clients and a completed race.


### 19.1 Overhead labels and actual online podium completed in code

`CGuiBehaviorKartMarker` event slot 2545940 -> 9d67e0 is identical in USA/EU.
Its +70 handle is the racer and +50 is the overhead text. Native manager
9d82d0 already creates markers for AI; the configure handler leaves their
text empty because no player is bound. The adapter now resolves racer +18
(driver), driver +40 (definition), definition +90 (hero key), and native
hero catalog lookup f9c610 (EU f9c600), using record +18's localized name.
This is the same name source used by native ranking labels. Human nickname
selection stays native. After native marker events, label color uses the
same four-team palette as portrait borders. Native projection, distance,
and offscreen hiding stay intact. The update.pak marker asset confirms +50
is Text_OnlineID001; +58 is raceLayout, not an AI visibility gate.

Adapter tests cover Yellow AI name/color and Red human nickname preservation.
The native marker vtable is signature-checked in both executable dumps.

Further podium evidence: generic online results consumer a298c0 writes three
character/customization/name slots into race manager +d98. It sets lobby
zone data +71 only after three valid slot records were built. This function
has no race-mode/Battle gate. eab300 is only the separate Battle winner/banner
presenter. No distinct four-character Battle podium was found or claimed.

Private team results now use that existing three-slot online ceremony. A
validated host snapshot orders racers by descending summed team points,
then by host finish order within equal totals. Only the podium builder's
slot selection is remapped; actual race finishing positions and per-player
awards are unchanged. With two teams of four, the three best finishers from
the winning team occupy the native slots. Smaller teams naturally leave
remaining slots to the next highest team, without duplicated characters.
Equal team totals preserve host finish order. Native podium requires at
least three valid participants, just as its original implementation does.

The adapter test executes all three patched native podium branch entries
and verifies that Blue's winning members 0,2,4 occupy slots despite the
individual finish order 0,1,2. Team snapshots do not set CUP-final state.
Pending podium is no longer cleared when returning to the lobby. Live
cinematic rendering/transition remains unverified until the next game run.

Final checks before executable build: C runtime adapter tests pass for both
regional layouts, Python asset/signature suite 12/12 USA and 12/12 EU,
`git diff --check` clean. Build command links existing out/gpu/libbbgpu.so;
it does not run any GPU build step. GPU SHA-256 before build:
`f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e`.

Executable-only build succeeded: `out/sacredfire`, 2064168 bytes, 2026-11-01T05:12:04.310153+07:00. GPU library timestamp and SHA-256 are unchanged after build. Ready for BB_PREBUILT=1 live testing; no claim of completed two-client or cinematic visual verification.


## 20. Live regression report: blocked selector, missing interim results (2026-11-01)

User's 05:21 screenshot and 051702 run invalidate the earlier implication
that selector input was fixed. All input methods still failed. Interim host
finish view had no right-hand timing/status list. Final results names lacked
team colors. Overhead names/colors and avatar borders are visible in the
screenshot. Log 051702 records four Blue and four Red assignments, not six
Red; individual score rows now use actual racer identities.

Interim finish regression came from substituting BattleTeam ranking without
also loading the Race_Ranking timing panel. Reuse the existing CUP interim
panel lifecycle for private team races: preload Race_Ranking, forward native
ranking events while awaiting final standings, then hide it on RaceFinished.
This keeps native finish times and unfinished status; it does not invent
placeholder times. RaceRankingSelectItem now colors +a8 (player name) from
bound racer +78 before the host final results exist. Its +d0 score override
only runs on score rows; plain timing rows retain native fields.

Removed the extra team-based three-slot podium ordering. User explicitly
requested no podium if Battle has no proven matching ceremony. The verified
Battle presenter eab300 supplies a team result banner, not a cinematic scene;
no dedicated Battle team podium was found in the inspected graphs/assets.
The generic online result consumer alone is not evidence for one. Private
team return now clears pending individual podium; existing CUP ceremony and
public/Battle behavior remain outside this change.

Selector correction: previous enable-controls call ran immediately after
OpenProject, which can precede streamed project initialization. Move it to
the native TeamSelect behavior's Initialize event, AFTER original native
initialization has completed, using owner +20's actual project. Capture private
selector eligibility before opening/countdown so removing the lobby component
does not defeat initial ready=false. Clear that latch on uninitialize, map
load, explicit leave, and subsequent ineligible selector opens.

Selector event slot 2543d68 -> 9c1250 now also logs native input signal,
controller-to-player mapping, ready-table lookup/value, selected team,
team count, and capacity. It does not poll raw input or force team membership;
the native change-team/network event path remains responsible for movement.
If no input line appears after an initialized selector, delivery is blocked
before the behavior. If a line appears, readiness/capacity/mapping can be
checked directly. Input success remains UNVERIFIED, not declared fixed.

Attempted to inspect the user's live processes, but both had closed before
the selector state could be read. A separate agent-started client was then
launched for reproduction; user instructed NO autonomous game testing.
Immediately terminated only that agent-started PID 438942 and unloaded its
temporary KWin focus script. No more game launch/input testing will be done;
user owns live verification. Unit/compile/signature checks continue offline.

Offline validation for section 20: C adapter tests passed for both regional layouts; Python game-asset/signature tests passed 12/12 USA and 12/12 EU. Added checks for deferred selector input enabling, eligibility surviving lobby teardown, unchanged public opens, interim timing event forwarding, and team name color before and after final host results. No live input/result verification was performed after the user prohibited autonomous game testing.

Section 20 executable-only build succeeded: 2073408 bytes, 2026-11-01T05:32:49.902445+07:00. GPU checksum remains f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e. User will perform next live test.


## 21. Selector disabled state is a counter, not a boolean (2026-11-01)

User clarifies that the selector itself is in disabled state. Static executable
inspection establishes `igGuiProject +0x48` as a nested disable depth:
`253990` increments it; `253970` decrements it, clamping at zero. Native
`a09190` enableControls calls 253970 exactly once, then enables GUI manager
input and refreshes the project. Therefore a single enable call is insufficient
when depth exceeds one. Earlier research describing this as a boolean was
incomplete. The actual depth in the user's failed run was not captured, so
nested depth is a supported failure case, not a claimed live diagnosis.

After original TeamSelect Initialize, private mode-1 selector now normalizes
its project depth to one and calls the native enable helper, resulting in zero
while preserving the helper's manager-input and refresh behavior. Normalization
runs at initialization only: future modal, close, and transition disables stay
native. Public/Battle selectors are unchanged. Diagnostic initialization log
records the previous disabled_depth for the user's next test.

Both native counter functions are checked byte-for-byte before hook installation
and against both game dumps. Unit test reproduces depth 3, verifies depth 0 after
private selector init, and verifies depth 3 remains intact on public initialization.
No game was started or controlled during this change; user owns live testing.

Section 21 checks: runtime adapter tests pass both layouts; asset/signature tests 12/12 USA and 12/12 EU; diff whitespace check clean. Executable-only build succeeded 2026-11-01T05:41:00.116043+07:00 (2073832 bytes). GPU checksum unchanged: f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e. User live verification remains pending.


## 22. Manual results, immediate selector, personal header, Single-only scope (2026-11-01)

BattleTeam_Ranking root contains three isInNetworkGame2 VSC branches: online
skips the legend, schedules the end-screen delay, and bypasses the manual
confirmation animation. Mark only these three nodes with one unused null pin;
115a740 (EU -10) selects their native offline/manual outputs only while a
private Single team ranking project is active. Other graph calls still use
native a90b10 network query. Global online state and lobby-return timer remain
unchanged. Back/confirm and DNF presentation still require user live verification.

Native lobby countdown eb7950 (EU -10) waits for remaining <= +2c before
opening TeamSelect. Private Single teams 2–4 now set that pending threshold to
remaining on the first countdown tick, leaving remaining +28 and the native
-1 one-shot latch intact. Both clients consume their existing synchronized
countdown; this is not a host-only menu-open shortcut. Streaming latency remains.

Team header now uses native WinsText material for personal WINS/LOSE/DRAW;
winner text is a separate TEAM WINS line, and a smaller cloned native text
placeable shows LOCAL TEAM points. The clone defaults hidden and is revealed
only by private Single results. Local team uses native controller 0 -> player
mapping. Unknown local team shows RESULT instead of falsely declaring a loss.
Tie header names tied leading teams; no winner-colored tint on a draw.

User limits all new team-race behavior to Single Normal/Itemless. CUP selection,
settings send/receive, and map load normalize Teams to native individual value 1.
CUP TeamCount selector locks its available options to individual and rejects
selection-change events. Race/selector/AI gates explicitly exclude CUP even
when its lobby uses race mode 1. Battle remains outside these runtime branches.

Offline checks: runtime adapter tests pass USA/EU; 14 Python asset/signature
checks pass in both regions. Added native opcode/vtable checks, marked-graph
routing tests, countdown threshold/latch/public tests, personal win/loss/draw,
extra-header visibility, and CUP option-lock tests. Executable-only build
succeeded; no game launch/input testing and no GPU build.

## 23. Battle friendly-fire/targeting audit — IN PROGRESS (2026-11-01)

User requests all behavior, including rocket AIM/lock-on, not only damage.
Track acquisition, impact filtering, area damage, dropped traps, and kart
contact independently. Do not generalize from a team-colored HUD.

Confirmed static facts so far:
- TeamCount selector 1293030 stores selectedIndex+1; value 1 is individual/FFA,
  not a single friendly faction for eight racers.
- CTeamFactionCollisionFilteringComponent callback f09c60 resolves contacted
  entity faction bits 22..33 and cancels contact through ba3320 when it matches
  component data +10. Registration alone does NOT establish which CTR items use it.
- Latest common PowerUp graphs for TrackingMissile, Bomb, Beaker, ExplosiveCrate,
  WarpOrb and InvincibilityMask call CKartNodes::applyKartDamage_VSC (112e490).
  That adapter calls 1041450 -> kart damage component e330e0. The shared adapter
  is not itself a same-team filter. e330e0 has a Battle-only scoring notification
  branch; that is distinct from preventing friendly damage or lock-on.
- Audit continues into native item components and target acquisition. No damage,
  aiming or collision behavior has been modified on this evidence yet.


Scoring comparison requested while friendly-fire audit continues:
- Current CUP awards 9/6/3/1/0/0/0/0 sum to 19; first place receives 9/19.
- Mario Kart 8 eight-participant table: 10/8/6/5/4/3/2/1 (39 total).
  Source: https://www.mariowiki.com/Mario_Kart_8
- Team Sonic Racing uses 15/12/10/9/8/7/6/5/4/3/2/1 for its twelve-racer
  team race, totaling individual position awards per team. Official manual:
  https://store.steampowered.com/manual/785260/
- Recommended for this eight-racer Single team mode: 10/8/6/5/4/3/2/1.
  Example A places 1/4/7/8 vs B 2/3/5/6: old A 10 vs B 9; recommendation
  A 18 vs B 21. This is a design recommendation, not an applied change;
  user has not selected a replacement yet. Existing CUP scoring remains intact.

Latest executable build: 2026-11-01 05:54:47 +0700, 2080640 bytes.
GPU SHA256 unchanged f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.

## 24. Approved Single team scoring and finish-order head-to-head (2026-11-01)

User approved the recommended eight-racer awards and explicitly selected the
best finisher among tied teams as the tiebreak. This supersedes section 22's
DRAW header and section 23's statement that scoring was not yet approved.

- Private Single Normal/Itemless team awards are now 10/8/6/5/4/3/2/1.
  Existing CUP awards remain 9/6/3/1/0/0/0/0; Battle stays native.
- Sum individual awards per team. If multiple teams share the highest total,
  scan the validated host finish order and choose the first racer belonging
  to one of those leading teams. An individual winner from a lower-scoring
  team cannot win this tiebreak. No extra race or client-specific decision.
- Results always have one winning team after a valid complete finish order.
  The local header is WINS or LOSE, followed by the winning TEAM WINS line,
  then the local team's points. Append (H2H) when the winning total was tied.
  Unknown local-player mapping still shows RESULT rather than inventing a loss.
- Duplicate result packets recompute the same snapshot, never accumulate points.
  Invalid/duplicate identities or invalid teams do not replace valid results.

Offline C adapter tests passed for both region layouts, including ordinary
22–17 results, a three-way 13–13–13 tie where RED finishes first, and a
11–14–14 result where BLUE finishes first individually but GREEN beats RED
on head-to-head. Row awards, winner/loser views, duplicate/malformed packets,
and unchanged CUP awards are covered. git diff --check passed.

Executable-only build completed 2026-11-01T06:04:02.978802+07:00,
out/sacredfire 2080488 bytes. GPU SHA256 remains
f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.
No game was launched or controlled; user will perform live verification.

Friendly-fire audit evidence added since section 23:
- Spawned rocket acquisition f2b210 (called by f2a9f0) explicitly skips racers
  sharing the owner's entity faction before selecting an enemy by angle and
  distance. This predicate has no Battle-mode gate. It establishes actual
  homing target acquisition, not the pre-fire HUD reticle or impact immunity.
- Rocket impact f28090 -> f281c0 and shared power-up event e35430 do not
  themselves compare owner/victim factions. Earlier physics filtering remains
  a separate question; absence in these callbacks does not prove friendly hits.
- ddae20 removes one entity ID from a physics-component bitset. Missile,
  beaker, bomb and shield initialization pass the owner's ID; this is owner
  exclusion, not evidence of a whole-team exclusion.
- Traced beaker f16eb0 -> contact f17570; bomb f1c980 -> f1a7c0;
  orb f0c510 -> f0ce40 / tick f0e4f0; shield f20ab0 -> e352b0;
  crate eb4240 -> eb3eb0; mask f17bc0 -> e3be90 / tick e3c170.
  These paths must be evaluated separately from missile target acquisition.
  Complete dropped-item, splash, clock, collision and HUD-lock conclusions
  remain pending; no friendly-fire changes are included in this executable.


## 25. Single team friendly damage and even team options (2026-11-01)

User requests an actual Normal team friendly-fire fix and only 2/4 team choices.
Native engine accepts 3 teams, but eight participants cannot divide evenly;
Single's choices are now No Teams / 2 / 4. CUP remains No Teams and Battle
keeps its native restrictions. Normalize old/synchronized Single settings of
3 to 2 before sending, after receiving and before loading. Reject a direct
3-team selection event as well as disabling index 2 in the available list.
No Teams remains available so the player can turn team mode off.

The shared CKartDamage component method e330e0 is reached by
CKartNodes::applyKartDamage_VSC -> 1041450, covering the item graphs traced
in sections 23–24. Hook before any native side effects. Resolve the attacker
handle and compare the target and attacker's native entity faction bits.
Reject damage with native result 1 only for different entities with the same
valid team in an active private Single race (2 or 4 teams, excluding CUP).
This protects AI as well as human teammates without using GUI names or player
IDs. Enemy damage, unassigned factions, world hazards, self-inflicted traps,
non-team races, CUP and Battle retain the original method. Ordinary kart
physics/contact is not removed. Item collision/consumption animations can
still occur even when their resulting teammate damage is rejected.

Both region binaries have the same 17-byte e330e0 prologue and address (no EU
shift here); the installer validates it before hooking and copies only complete
non-relative instructions. Asset tests check both real binaries. Native missile
acquisition already rejects same-faction targets; that aim predicate is preserved.
No claim is made that the pre-fire HUD reticle has separately been changed.

New offline tests cover all ten native damage-kind values for teammates,
enemies, unknown factions, null source, self hits, fourth faction, No Teams,
and exit from the private race. Team option tests cover the 3 -> 2 migration,
availability, rejection of a direct 3-team event and passthrough for 4 teams.


Clock follow-up: CSpawnedNtropyComponent f1f490 iterates racer entity handles
and calls f1fb50 per target, excluding only the owner. f1fb50 checks damage
eligibility e346f0 but applies its slow effect directly with e3a650 and sends
power-up messages through e35430; it bypasses e330e0. Added a second narrowly
scoped hook at f1fb50 (EU -0x10), rejecting teammates before the slow effect,
recipient-list insertion or hit messages. Enemy and non-team execution uses
an instruction-validated trampoline. Tests exercise friendly/enemy Clock targets
and passthrough after leaving the private race; actual prologues checked in both
region dumps. This resolves the separately identified Clock gap rather than
assuming the shared damage hook covers every item effect.

Final checks: C runtime tests pass USA/EU including native Battle damage/Clock
passthrough and retention of Battle three-team settings. Python asset/signature
tests pass 14/14 in each region. git diff --check clean. Executable-only build
completed 2026-11-01T06:11:15.215154+07:00, 2091664 bytes.
GPU SHA256 unchanged: f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.
No game process was launched or controlled. User live testing remains pending.


## 26. Remove the 3-team option in Private Single only (2026-11-01)

User supersedes the disabled-option approach: remove it from the visible list.
Added a shared race-options wrapper installed after online hooks and also when
online is off, but only modifies Private Single Normal/Itemless. Offline and
Battle return directly to native code. It transfers the native "4" string into
index 2, releases the removed "3" string, and shortens both label and enabled
lists to three entries: No Teams / 2 / 4. The display index 2 is mapped back to
native index 3 only while dispatching its selection event, then restored.
Actual teamCount remains 4, avoiding accidentally selecting three teams.
Battle's full native list bypasses this wrapper. Existing private CUP lock
still applies. Unit tests cover list removal/reference ownership, index mapping,
repeat updates, two-team selection and native Battle three-team selection.
New wrapper has not yet been live tested or built into the executable.


## 27. Feedback at 06:24: input routing, readable header, portraits

The user corrected the option scope: offline three-team settings must stay
native. Tests now cover this passthrough. Battle remains native too.
Latest saved log 20261101_061247.log reports disabled_depth=0 on both selector
initializations, but no input event callbacks. A ready-table-only explanation
is insufficient. Native GUI dispatcher 24acf0 also checks project +34 bit 0x20
and controller mask +44, independently of disable depth +48. The initialize
callback now activates project input through 24ac80 if necessary and supplies
all local controller bits when the inherited mask is empty. This is one-time
initialization, not an every-frame override of modal/closing controls. Ready
initialization remains false for Private Single. Live resolution is unconfirmed.

97f160 expects a controller BITMASK, not index zero. The result header now uses
the result project's controller mask, falling back to primary mask 1. This fixes
the source-level cause of RESULT appearing instead of local WINS/LOSE. Winner
team text is white; heading/points positions are raised and points scaled down.

The BattleTeam row's native portrait field +98 was null. It now references a
clone of the RaceRanking portrait placeable, with explicit RaceRanking asset
dependency, proper RHND/ROFS fixups, and insertion in the row child list. Native
9b87e0 binds each racer's character texture. The portrait starts hidden and is
revealed only by the private team row callback. Visual spacing and input still
require the user's game test; no live game is run by the agent.


## 28. AI-first finish countdown, solo and teams

Native f34430 counts finished humans separately from all finishers. Online
starts the race-end timer only when the human count is positive. Thus an AI
finishing first alone does not start the countdown in the unmodified Single
path; this is established from code, not inferred from the screenshot.

The existing Private CUP trigger change at f34575 now also applies to Private
Single Normal/Itemless, both No Teams and team races. The opcode changes the
trigger from test ebp,ebp (human count) to test edi,edi (all finishers). Native
network event dispatch, one-shot timer latch, timeout duration, DNF processing
and all-human completion remain intact. Battle, public and offline retain
native behavior. Offline Arcade uses a separate completion branch and does not
use this online timer dispatch; no claim is made that it now gains a countdown.
Tests verify the selected opcode for solo, team, CUP and reset/Battle in USA/EU.

The scoreboard portrait clone now has a second child using the native
Octane_Race_Leaders contour sprite, colored by team at binding time. Header
winner text is white, with points moved above racer rows. Asset tests verify
both portrait and contour names, handles, sibling links and dependencies.
User live testing is still required for visual spacing and selector input.


## Latest user regression / load failure evidence (2026-11-01 07:10)

User reports selector still disabled and both finish-time and final scoreboard
missing. Existing init log shows flags 0x61, controller mask 0xff, disable depth
0, but no selector input callbacks. These init values do NOT prove input works;
GUI dispatch also checks project owner +e2 and later disable depth. Do not mark
selector fixed from initialization alone.

Latest supplied play log `20261101_065525.log` lines 5276–5279 records failed
load of `octane_battleteam_ranking:StreamingUI` because dedicated archive
`/app0/archives/octane_battleteam_ranking.pak` is absent. The asset exists in
race-domain archives, so the failing streaming request/residency path requires
investigation. Current runtime log says ranking=loaded when it only has an
allocated handle; that is not proof of completed streaming. Final scoring math
can succeed independently of a loaded UI. No new speculative UI patch applied
in this continuation; selector and panel regressions remain OPEN.


## Online panel residency patch (2026-11-01)

Extended the Online menu package dependency closure with the three native GUI
projects needed by private team races: Race_Ranking, Race_Leaders and
BattleTeam_Ranking. Source types/order come from the native race package;
recursive TDEP closure includes materials, language and textures. Files owned
only by the race/startscreen archives are preserved via the existing loose-file
map, because those domain archives may be released during transitions. Patched
update/update2 assets continue to come from their overlay rather than an original
loose copy. This targets the observed fallback request for a nonexistent standalone
BattleTeam ranking archive. The runtime now logs a handle as "requested", not
"loaded". Actual display is still pending user verification.

Checks: preparation succeeds, dependency closure has no duplicate rows and
contains all three panels and their materials/portrait contour. 15 Python tests
pass against both USA and EU dumps; online runtime C checks pass. No game launch.

Additional Battle state trace: slot handler 9c5dc0 accepts configure-team and
PlayerReady events. 9c6cc0 uses matching player handle and event+20 to update
slot+70, then plays +50 (unready) or +58 (ready). Slot update 9c60b0 binds players
by player+e4 versus slot+60 and reads the shared ready map through slot+80;
empty slots and first-bind/changed-player states have separate animations.
This does not establish that the user's gray slot is a hardware input failure.
Native cancel 9c3bf0 emits unconfirm if ready; 9c2db0 clears the map and emits
PlayerReady(false). The root VSC provides two native legend entries. Continue
tracing their creation/visibility; selector regression remains unresolved.


## Concrete Team Selector relock found and patched (2026-11-01)

The relevant native lobby path uses **open2**, not openByName. In update2 root,
node 4360 opens the TeamSelect project; its opened callback is exported method
`method__id_qp45dard` (7050), entry 4160. That callback invokes disableControls
at 4198 on the freshly opened project accessor 40b8, then close2 at 41e8 on the
old lobby. Thus resetting disable depth at TeamSelect Initialize was too early:
the opened callback disables it AGAIN. This is concrete graph evidence, independent
of guessing from absent input logs or the gray screenshot.

`team_selector_callback` locates the callback by the native name/object tables
in both update and update2 (layouts differ), verifies its first action is native
disableControls, and adds one unused marker pin. Runtime wraps 110b9c0 (EU -10):
only this marked call, while Private Single TeamSelect is active and not CUP,
returns without disabling. The engine continues the existing callback chain,
including closing the old lobby. Battle, public/non-private state and all other
disableControls calls retain the original function. Native ready insertion remains
false for Private Single; X confirm / O unconfirm handlers and network requests
remain native. No per-frame force-unready or arbitrary input injection is added.

Asset checks verify only the intended action changes and original pins/next action
are retained in both archives. Runtime tests cover marked private callback,
unmarked disables, inactive selector and Battle fallback in USA/EU layouts.
These pass. User gameplay verification remains necessary; this finding supersedes
the earlier input-only suspicion, but does not claim a live test was performed.

Final executable-only build: 2026-11-01T07:30:36.170374, 2132160 bytes. Includes selector callback relock fix, difficulty cursor fix and connected alternate-route tuning. CPU unit checks pass; asset checks cover USA/EU including both lobby-root versions. GPU SHA256 unchanged (f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e). No live gameplay tests.

## Team Selector confirmed; ranking dependency correction (2026-11-01)

User explicitly confirmed: "team selector sudah work". Latest captured selector
log also has ready=0 and left/right signals changing team. Treat this fix as
user-verified; do not reopen the hardware/input theory.

The same session still fails to open the team ranking project: the loader requests
`octane_battleteam_ranking.pak`, which does not exist. Team scores are calculated
(BLUE 21, RED 18), so score calculation is not evidence that the GUI loaded.
The previous Online-package preload alone did not fix this, per user report.

Removed newly introduced whole-GUI dependencies on Race_Ranking and Race_Leaders
from the patched BattleTeam_Ranking. Portrait and contour sprites are now local
igGuiSprite exports in the BattleTeam namespace, with copied material EXID handles,
export name hashes, paired root/name arrays and material TDEP references. This
removes cross-project sprite resolution from the loading path. Native source sprite
RHND values are EXID indices, not EXNM indices. Tests verify the copied material
identifiers against the source, and absence of the two whole-GUI dependencies.

This is a candidate correction for failed project loading, NOT proof that the
missing-archive fallback was solely caused by these dependencies. No game was
launched or controlled. Results now log whether ranking/finish handles actually
resolve, rather than implying a requested handle means successful load. Both
finish forwarding and final scoreboard still require user verification.

Validation: 16 Python asset tests each on USA and EU; USA patch preparation;
C online-mode tests including interim finish-event forwarding and team results.
All pass. Generated validation assets are in /tmp, leaving running games alone.

Executable-only build completed 2026-11-01T07:58:18.712476, 2142776 bytes at out/sacredfire. GPU hash unchanged. User launch with BB_PREBUILT=1 and BB_PROBE="$PWD/out/sacredfire" regenerates updated Online assets through run.sh. No gameplay test performed.

## Follow-up: missing ranking archive and intro order (2026-11-01)

User confirms selector works. Leave that flow intact. Latest report: intro cannot
be skipped; finish panel and final scoreboard still absent. The 07:59 log still
requests missing `archives/octane_battleteam_ranking.pak`. Logging a non-null
resolved handle does NOT prove a loaded visible GUI: native 987380 can return a
future object while StreamingUI is pending.

Changes awaiting user gameplay verification:
- Open the additional finish panel at the original ranking-open point, BEFORE
  native intro setup, instead of after the whole race_ui call. Preserve native
  primary ranking/minimap/intro order. This addresses a possible input-stack
  interruption; no claim of a confirmed live intro fix.
- Forward ONLY CGuiEventLocalRacersFinished to the additional finish project.
  Previously all events except final-race notification were forwarded, including
  unrelated initialization/input/animation events. RaceFinished still hides it.
- Native985510 requests generated/UI/<project> when a project is not resident.
  Generate actual standalone BattleTeam_Ranking and Race_Ranking archives via
  the existing graphics_tab archive writer and loose-file overlay. Include native
  chunk_info package at packages/generated/ui/<project>_pkg.igz, patched project/
  VSC and its race-package dependency closure. Never modify the user's game dump.
  Preserve source package order, native loader kinds and permanent dependencies.
  Existing package preload remains; fallback now has a real archive if released.

Tests read generated archives back, verify patched VSC bytes, root/package paths,
material dependency presence and every package row's payload. Archive ordinal's
low byte is the high offset byte; generated ordinals use index<<8. Runtime tests
cover finish-before-intro ordering and selective event forwarding. These are
static/unit checks only; finish/score visibility and intro skip need user testing.

CPU executable rebuilt: 2026-11-01T08:25:40.611114+07:00, 2148608 bytes at `out/sacredfire`. No GPU build and no gameplay launch. Runtime C checks pass; 17 online asset tests pass for both USA/EU dumps; full USA online preparation succeeds. Developer ghost decoder validates all 40 developer references.


## Native finish panel restored; scoreboard opened at final results (2026-11-01)

User confirms intro skip now works, but both panels still fail. Saved 17:20 log
contains `result panels ranking=unresolved finish=resolved`. Scores were computed
correctly; the previous extra finish project only received its local-finish event
through the substituted scoreboard's dispatch, so an unresolved scoreboard also
prevented the interim panel from receiving that forwarded event. The log alone
does not identify why the substitute scoreboard stopped resolving.

Changed lifecycle: Team Single now retains native `Octane_Race_Ranking` in native
race UI slot +60 throughout racing. Its own local-finish events take the native
path, without depending on the BattleTeam project. When the final host results
arrive, open BattleTeam_Ranking immediately before native result processing and
put that result into +60 so the native end-race flow selects the scoreboard.
The finish panel is hidden at that transition. Existing team row colors, points,
header and navigation patches remain. CUP's separate finish flow is unchanged;
the additional panel is still opened before intro only for CUP.

C fixtures now use separate finish/scoreboard objects and assert native finish
slot preservation before results, exactly-once finish event delivery, deferred
scoreboard opening and slot replacement on results, plus existing score/header,
CUP, Battle and selector checks for USA/EU. These checks pass. This is a candidate
runtime fix requiring user gameplay confirmation, not a verified visible panel.
No game was launched and no GPU build was performed.

CPU build completed: 2026-11-01T17:45:30.445692, 2188064 bytes at `out/sacredfire`. GPU unchanged; no gameplay launch.

## Guest Ready delivery follow-up

Native 9c4160 sends the guest confirm request through a4d170. Receiver 9c59e0
creates the GUI confirmation event and broadcasts through 248d10 to the current
GUI layer. Selector 9c2340 performs team validation, updates its ready table,
emits the native player-ready visual event and retains the host acknowledgement
path. Private Single opens the selector from a lobby callback, so a missed GUI
layer delivery is a candidate explanation; it is not confirmed by a live trace.

Added a scoped fallback around that native GUI broadcast: retain a handle to the
active private team selector, observe whether its native handler received the
same Ready/Unready event, and deliver to its project only when native traversal
missed it. Release the handle on uninitialization/leave/map change. No synthetic
ready state or new network packet is added, and native team validation remains.
Bounded scope prevents changing Battle and unrelated broadcasts. Diagnostics
report rerouted network ready/unready and whether delivery reached the handler.

C fixtures for both regional layouts cover missed delivery, no duplicate when
native delivery works, unready, unrelated events and stale-project cleanup.
These pass but do not simulate two live clients or prove network requests arrive.
The guest Ready fix remains a candidate until the user's host/guest gameplay test.

CPU build completed: 2026-11-01T17:59:57.971867, 2197984 bytes at `out/sacredfire`. GPU SHA256 unchanged: `f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e`. No gameplay launch.

## Private selector Back is Unready only (2026-11-01)

User reports host Ready reaches the guest, while guest Ready does not appear on
either client. This is asymmetric; it does not establish whether the guest's
request was sent, received, rejected by native validation or missed GUI delivery.
The preceding scoped native broadcast fallback remains the candidate repair;
no additional speculative transport or locally forced ready state was added.
Latest saved 18:00 log has no selector input trace, so it cannot resolve this.

Native input dispatcher 9c22a0 maps signal 8 to Ready (9c4160) and 9 to Cancel
(9c3bf0). Cancel reads the ready table: true emits Unready and its network event;
false emits the close/back event. Private Normal/Itemless Single now consumes
signal 9 when the local ready entry is false or cannot be resolved. When true,
Cancel follows the native unready path. Ready and team movement remain native.
Battle is unchanged. This applies equally to host and guest and keeps players
on the selector until the existing match-start transition.

Both regional C fixtures pass checks for not-ready Back suppression, ready Back
forwarding, unresolved ready entries, unchanged Ready/team movement and native
Battle Back, alongside Ready/Unready fallback/no-duplicate/lifecycle checks.
No gameplay was run. Guest Ready synchronization still requires user validation.

CPU rebuild: 2026-11-01T18:03:34.825661, 2198128 bytes. GPU checksum unchanged; no gameplay launch.

## User confirms selector + finish; scoreboard still absent (2026-11-01)

Team Selector and the interim finish panel are confirmed working by the user.
Latest 18:00 log reports final scoreboard opened=1 resolved=1 and valid team
scores, despite no visible scoreboard. Loading alone is therefore insufficient.
BattleTeam_Ranking's RaceFinished graph sets its input-ready flag and starts
placeable/animation flow. Opening only when the final network snapshot arrives
can miss the earlier native RaceFinished event received by the finish panel.

When the native finish panel receives RaceFinished, open the team scoreboard
and forward that actual event to its resolved project. If it was already opened
by the results snapshot, forward to that same project. Track first delivery so
native broadcast traversal cannot restart the animation with a duplicate event.
Do not invent a GUI event or change scoring, selector state or interim timing.
The host snapshot still supplies team points and refreshes names/header afterward.

C fixtures exercise RaceFinished before the final snapshot, scoreboard event
forwarding exactly once, and duplicate native broadcast suppression. Both regional
layouts pass. This is a candidate visibility fix; no game was launched.

Validation completed: C race-options, online-mode (both regional fixtures), TT guidance tests, and all four Python reference tests pass. Export regenerated 160 references. CPU rebuild: 2026-11-01T18:12:25.630018, 2199680 bytes. GPU SHA256 remains f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e. No gameplay launch.

Latest CPU build (includes power-slide cycle follow-up): 2026-11-01T18:14:31.116046, 2200264 bytes. C tests pass; GPU checksum unchanged. No gameplay launch.


## 2026-11-01 — exclusive ownership of the final team result flow

User screenshot still shows Wumpa Report for host and no scoreboard for guest.
Latest available log is 20261101_184807.log: final scoreboard opened=1,
resolved=1, forwarded RaceFinished, valid eight-row totals. Thus the earlier
missing-event candidate is insufficient; do not claim visible UI from these logs.

Inspection found the previous cup_project_event forwarding executed
original_project_event on Octane_Race_Ranking BEFORE forwarding RaceFinished to
BattleTeam_Ranking. The old project's RaceFinished graph (5c50 -> 5cd8 ->
5d10, with its own network/end-flow branches) was still active. Hiding its root
does not disable graph event execution or scheduled end-flow callbacks. Two
projects therefore received the same final transition and could advance the
end-screen flow independently. This is a code-level defect consistent with the
screenshot, not proof that it explains every missing-panel symptom.

Transfer the final event to the resolved team project before executing the old
project handler. Successful transfer returns without starting the old graph's
RaceFinished flow. Duplicate delivery to either project is consumed. Existing
local finish events remain native; Battle/CUP stay on their existing paths.
If team project opening/resolution fails, retain native handling as fallback.
No synthetic event, permanent visibility forcing or timer extension added.

C regression now checks old finish handler receives zero final events while the
team handler receives exactly one, including repeated broadcasts to both. It
previously only checked delivery to the new project and missed old-project
execution. Both regional fixtures pass. No gameplay launched. Visibility,
CONTINUE navigation and Wumpa transition still need user validation.

CPU-only build completed 2026-11-01 18:58:31 +0700: out/sacredfire, 2211112 bytes. GPU SHA256 unchanged (f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e). git diff --check passes.


## 2026-11-01 — prepare final scoreboard during native race UI creation

User again confirms private-team final scoreboard absent. New 200410 log shows
activated previous patch: opened/resolved team project, exclusive RaceFinished
ownership (interim flow suppressed), valid BLUE/YELLOW 14-point tie resolved by
head-to-head, and eventual lobby return. Therefore stopping duplicate flow did
NOT establish visibility. Previous claims must remain limited to event routing.

Native e9ba30 creates ranking projects during race UI construction. Our team
substitute instead created BattleTeam_Ranking from the RaceFinished handler and
immediately delivered its first event. A non-null project handle did not verify
that VSC initialization, children or animation bindings had completed. This is
a lifecycle discrepancy; available logs do not prove precisely which internal
initialization/visibility gate dropped the event.

Changed creation timing: at the native Octane_Race_Ranking open call, keep that
project as the live finish panel and request BattleTeam_Ranking with the same
native options/player/delay, before returning to intro setup. Hold its reference
through the race. Do not put it in UI+60 until the final transition. At results,
activate the existing resolved scoreboard; never create a new project from its
first RaceFinished callback. If unresolved, retain native finish flow and report
the failure. No frame-by-frame visibility forcing or fabricated GUI events.

Handle either native broadcast order: score-first activates/handles once;
finish-first activates/forwards once. Subsequent delivery to either does not
restart the final graph. Local finish events still go to the original race panel.
Native Battle/CUP branches are unchanged. Existing archive/dependency generation
remains necessary; no content patches changed in this turn.

Regression asserts scoreboard creation during race UI, finish-slot retention
through racing, no additional creation during results, both broadcast orders,
and one final event with no interim end-flow event. Both regional fixtures tested.
No gameplay launched. Actual visibility and intro/input ordering still require
user gameplay validation; this log does not justify claiming the panel fixed.

CPU rebuild completed 2026-11-01 20:14:13 +0700: out/sacredfire 2219760 bytes. Online-mode regressions pass for USA/EU. GPU SHA256 unchanged (f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e); git diff --check clean. No game launched.


## 2026-11-01 — follow the native Battle scoreboard owner and result pipeline

User again reports no private-team scoreboard. The latest relevant private-team
log is 201522 (newer202203 has no TEAM records). It reports prepared scoreboard
UNRESOLVED with the finish panel resolved; pre-opening alone did not fix it.
Prior event-timing and exclusivity changes were insufficient. Do not treat their
unit tests or requested handles as visual verification.

Re-read/decompiled native e9ba30 and ea4540/eab300. Native race ranking is owned
in race UI+60; Battle/BattleTeam_Ranking is owned in UI+68. ea4540 separately
processes these slots. Its Battle path resolves +68, invokes eab300 to populate
and dispatch BattleArenaResult, dispatches RaceFinished (1100770 creates type
2ac02a8), then dispatches the cached event at UI+40. The race +60 path uses a
different population/order path. Our substitution into +60 bypassed the native
Battle population and companion events. This is an established integration
mismatch, independent of the earlier visibility hypotheses.

Correction: after native race UI construction, retain the prepared team project
in native UI+68. Keep normal Race_Ranking+60 for the live finish/time panel.
When final RaceFinished activates the team project, retire +60 rather than
replacing it with BattleTeam_Ranking. ea4540's native +68 path now owns the actual
Battle result population and event sequence; its later +60 race-only path is
skipped. Retained finish handle still supports pre-final local finish events.
No manual invocation of the whole Battle game mode, fake Battle result payload,
or forced visibility flag. Native Battle/CUP mode selection remains unchanged.
Existing private team totals/rows are refreshed after native result events so
native Battle's winner text does not replace the private race scoring rule.

Tests now model +68 independently from +60 (fixture expanded to0x70), assert
both slots before finish and retirement of +60 afterward, and verify delivery
of arena-result/finish/context companions while suppressing only duplicate finish
events. Both regional runtime fixtures are checked. Actual rendering and native
resource lifetime still require user gameplay validation; no game was launched.

USA/EU runtime regressions pass. CPU build completed 2026-11-01 20:27:25 +0700: out/sacredfire 2220984 bytes. GPU SHA256 unchanged (f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e); git diff --check passes. No gameplay launch.


## 2026-11-01 — remove scoreboard avatars and trace project resolution

User reports scoreboard still absent and recalls it working before added avatars.
Latest private-team log 20261101_202951 confirms ranking=requested at race UI
construction, then prepared scoreboard unresolved at results; finish panel resolves.
Totals are valid BLUE21 / RED18. This proves failure precedes row rendering, but
not whether the injected asset structure/material dependencies caused resolution
failure. Do not describe the avatar hypothesis as confirmed.

Removed avatar and contour placeable injection from team_ranking_header, the copied
sprite exports/material EXIDs/dependencies, and runtime avatar reveal/recolor code.
Native BattleTeam row hierarchy and character binding remain untouched (binding
+98 remains null). The additional hidden team-points header is retained, as are
private team scores, native event pipeline, finish panel, team selector, and race
HUD borders. Removed unused cross-project sprite-copy helper and its asset reads.

Added bounded scoreboard event tracing (32 deliveries per race): project request
return, UI ownership assignment, activation, before/after event delivery, forwarded
finish and native results return. Trace includes handle vs resolved object/root,
root flags, resolved finish and native Battle slot, opened/finished state, result
count, event receiver and event type. This distinguishes resource resolution from
native graph/visibility progression without forcing visibility or synthetic events.

17 Python asset tests pass using local game data. Regression verifies unchanged
row bytes, exports, EXID/EXNM/TDEP data and absent avatar markers. USA/EU runtime
fixtures pass with new tracing. A full asset-generation check caught a removed
race UI index still needed for the finish-panel archive; restored that lookup.
No game launched or controlled; no GPU build. User visual validation remains pending.
run.sh regenerates online assets even with BB_PREBUILT=1, so the user's configured
BB_DATA_DIR receives the avatar-free patch at next launch.

Full asset preparation succeeded; generated BattleTeam streaming archive was
reopened and verified to contain no custom portrait markers and a null native
avatar binding. CPU build: 2026-11-01 20:41:11 +0700, out/sacredfire 2222536 bytes.
GPU SHA256 unchanged: f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.
git diff --check passed.


## 2026-11-01 — plain Highscores-style avatar using the scoreboard's own sprite

User confirms avatar removal restored scoreboard visibility. Requests avatar only,
without contour/frame, like the Time Trial Highscores list. No gameplay launched.

Inspected native HighscoreListItem: its character portrait is a64px minimap sprite
(placeable b90), distinct from the driving-archetype icon. Native ranking9b87e0
already binds the racer's character minimap portrait through behavior+98, using
c02e90 and the placeable's sprite material. Native 24c720 resolves that sprite's
handle. BattleTeam itself exports an unused igGuiSprite `Portrait001`; it was not
referenced by any original EXNM handle. Therefore no copied sprite export,
material EXID, external GUI project or new TDEP dependency is necessary.

Append one hidden placeable, based on a simple native local leaf. Bind its asset
to existing local Portrait001 through one EXNM entry; bind row behavior+98 to it.
Position80,41;64px square scaled0.75 (48px), centered, ahead of name beginning120.
No frame, no portrait tint. Native row handler supplies each racer's character;
private-team callback reveals only the explicitly marked OCTR_TEAM_AVATAR after
a racer is bound. Battle keeps its native team-row handling and hides portraits.
Race HUD/finish panel are unchanged.

Also found a concrete structural defect in the removed avatar patch: it treated
the last child-table member5c70 as the end of a sibling list. Native +50/+58 links
are the flattened project order;5c70 originally points forward to5d90, which points
to52d0 and the rest of the subtree. The old append replaced that forward link and
ended the chain at the added border. New insertion is between5b50 and5c70 with
both neighbors repaired, preserving all subsequent nodes. This is an established
old asset defect, but there is no isolated gameplay experiment proving it was the
sole cause of unresolved project loading (the old dependency changes also existed).

17 Python asset tests passed with local game dump. They verify unchanged EXID,
TDEP, export count and sprite count; local portrait binding; hidden initial state;
exactly one added child; both directions of insertion and identical old forward
traversal after filtering out the new avatar. USA/EU runtime fixtures pass and
verify private-team reveal alongside unchanged rank/score behavior. Full prepare
successfully regenerated streaming archives and overlays. No GPU build; no visual
success claim. run.sh regenerates assets for the configured BB_DATA_DIR at launch.

Generated streaming archive reopened and avatar/chain/no-frame assertions passed.
CPU-only build completed 2026-11-01 20:57:51 +0700, out/sacredfire2224448 bytes.
GPU SHA256 unchanged: f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.
git diff --check passed; visual validation remains with the user.


## 2026-11-01 — finish-style row pitch and engine icon

User requests list spacing like finish panel, with avatar, engine-type icon and
team-colored name; sprites must retain their original color. Header/scoring flow
are outside this change.

Compared native UI assets: BattleTeam row template53f0 height86 vs RaceRanking
47d0 height73. Both list assets have vertical spacing-9; visible item count differs
4 vs8. Mark the BattleTeam list placeable (e30) and adjust its resolved list/template
only in the private-team runtime path, before results graph dispatch. Set height73
and visible8; leave serialized Battle dimensions/count unchanged. Effective pitch
becomes64 vs77, saving91 local units across seven intervals so row8 sits higher.
No changes to title, points, ordering, score rules or navigation.

Add native behavior+a0 engine sprite alongside existing +98 portrait. Reuse local
unused Portrait002 export, with no new sprite/material/dependency. Native9b9900
binds engine-type icon. Match finish-panel columns: avatar x80,y53,64px at.85;
engine x131,y50,80px at.45; name x158. Only name receives team tint. Engine/portrait
retain native material colors. Insert the engine between avatar and existing5c70,
preserving bidirectional flattened object order and all original children.

17 Python asset tests and USA/EU runtime fixtures pass, including engine binding,
no extra external dependencies/sprites, complete retained object chain, unchanged
serialized Battle list dimensions, private row-pitch/count and name placement,
engine reveal and unchanged white color. Full asset generation succeeds. Actual
screen fit/rendering needs user validation; no gameplay launched or GPU build.

CPU-only build completed 2026-11-01 21:04:47 +0700, out/sacredfire2231080 bytes.
Race-options Clang ASan/UBSan passed; runtime and asset regressions passed.
GPU SHA256 unchanged: f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.
git diff --check passed. No gameplay launched; user validation pending.


## 2026-11-01 — fix touching rows, sprite alignment and explicit POINT labels

User screenshot211339 shows no vertical separation, list still too low, avatar/
engine icons appearing below their intended racer, and requests explicit10 POINT,
8 POINT etc. Scope is scoreboard layout/text only; AI/scoring rules unchanged.

Compared copied null-template5550 with native finish portrait4a50: alignment fields
+a0/+a4 are2/2 in the null template but0/0 in the portrait (and native engine56b0).
The prior patch copied coordinates, pivot and scale but omitted these alignment
fields. Set both to0 before cloning the engine; use the native portrait rotation.
This corrects inherited alignment rather than hiding the displacement with another
arbitrary Y offset. Sprite materials/colors and native racer binding are retained.

The previous73-unit item height changed pitch to64 while retaining BattleTeam's
larger background artwork. That compacted rows until backgrounds touched/overlapped.
Restore the original86-unit item height (-9 spacing gives77-unit pitch), retain8
visible rows, move the marked private-team list upward with local Y=-7. This aims
to use the empty area below the total-points heading and retain space above the
Continue footer. Header/navigation and serialized native Battle layout unchanged.
Actual final margins still need user screenshot validation.

Per-racer score strings now use the requested literal format `%d POINT` for all
awards (including zero). Keep native right alignment; scale this score placeable
to.75 on X/Y to accommodate the longer label without crowding the name. Native
position rank stays numeric. Tests verify all eight award strings, duplicate and
malformed-order handling, restored pitch, upward position, text scale, absolute
sprite alignment and preserved project chain/dependencies. No game launched.

Validation: USA/EU runtime fixtures and17 Python asset tests passed. Full asset
preparation succeeded. CPU-only build2026-11-01 21:22:29 +0700, out/sacredfire
2233072 bytes. GPU checksum unchanged:
f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e.
git diff --check passed. User visual validation pending; no gameplay launched.


## 2026-11-01 — lift team results and move return countdown beside panel

Screenshot212928 confirms the restored row spacing and sprites, but the return
countdown overlaps WINS and row8 meets the Continue footer. Move private-team
return timer root X from native1601 to1050, retaining its native Y110 and
countdown behavior. Apply only while the private-team ranking project exists;
ordinary CUP retains its timer position and NEXT RACE label behavior.

Lift all three result headings by15 local units (WINS -19, winner -4, points9),
and the player list by the same15 (-22). Keep original86 row height,77 pitch,
eight rows, portraits, engine icons and POINT labels. Mark transforms dirty.
USA/EU runtime fixtures pass, including timer placement, unchanged ordinary CUP
timer position, heading/list offsets, and all eight scores. Visual fit still
requires in-game verification; no gameplay launched.

CPU-only build completed 2026-11-01T21:37:27.047547+07:00; out/sacredfire 2236336 bytes. GPU SHA256 unchanged (f84b449c9fe9d9fc86c90095918df660a1094d421caf32401a60fe23f8d31f2e). git diff --check passed.


## 2026-11-01 — keep row one inside clipping parent; native DNF opacity

User reports row1 clipping after raising the list, and requests native-style
transparent rows for retired/non-finishers. BattleTeam list e30 is inside ce0
(flags401002, scissor wrapper, local scaleY10.874557). Moving only listY from-7
to-22 displaced content relative to that wrapper. Restore listY-7 and move its
parent fromY303 to140, approximately the same net shift while carrying the
clipping region with it. Keep8 visible rows and original77 pitch. Native Battle
serialized dimensions remain unchanged; runtime adjustment is private-team only.

Native RaceRanking NotFinished tag500 selects animation6dd0 on row46b0. Its
float-track property2 keyframe6e88 has value0.6. Verified native opacity accessor
269490/setter2694a0 reads/writes placeable+e0 and marks bit1000 dirty. Apply the
same0.6 opacity to the marked scoreboard avatar's parent row52d0 when racer+8c
is false; restore1 for finishers. The parent covers background, name, rank, score,
avatar and engine together without destructive color/material changes or
compounding alpha on descendants. Native finish-panel rows are not marked and
retain their own animations. Native9b9280 also uses racer+8c for race result state.

USA/EU runtime fixtures pass: parent/list positioning, eight unchanged scores,
DNF opacity, finished-row restoration and repeated binding. Rendered margins
and appearance still require in-game verification.

CPU-only build 2026-11-01T21:51:58.636960+07:00; GPU checksum unchanged.
Online USA/EU fixtures and AI Clang ASan/UBSan passed (existing literal-pointer
comparison warning suppressed). git diff --check passed. No gameplay launched.


## 2026-11-01 — No AI team Start gate

Private lobby ButtonReady graph calls COctaneLobbyNodes::canStartGame_VSC
(USA12108f0/EU12108e0). Its native core f507f0 checks connected/ready counts,
not the eight-human requirement. Replace that small graph adapter, preserving
its native success/failure branches whenever our capacity rule passes. For
private race teamCount2/4 and private settings useAI(+18)==0, read the live
player-manager list count(+10 -> +0c) and stop before either continuation when
under eight. Battle and non-private sessions keep native behavior.

Use native network-message helper a548e0(message,NULL), the same helper reached
from showNetworkErrorStatus -> a61ec0. It interns/copies the text, creates the
native OK dialog through a056f0/9f9790 and the configured GUI project. No OS
dialog, deferred stack pointer, network error fabrication or custom overlay.
Message states eight players required, current count and wait/enable AI options.
Hook checks native entry signature before installation; no stolen RIP-relative
instructions are executed. USA/EU fixture matrices cover 0..8 players, teams
1/2/4, AI on/off, private/non-private and native readiness rejection. Passed.
Live multiplayer and actual popup rendering remain unverified.


## 2026-11-01 — correction after screenshot 215842

Supersedes the previous full-eight gate and parentY140 layout. No AI must
permit balanced lobby counts: two teams at2/4/6/8 humans, four teams at4/8.
Reject counts below team count, above8 or not divisible by team count, using
the same native dialog with the appropriate allowed counts. Existing readiness
validation is preserved. USA/EU fixtures cover every count0..8 for both rules.

The screenshot shows parentY140 places row1 behind winner/points headings,
with unused space below row8. Set parentY260 (+120 native units, about60 pixels
in this split-screen screenshot), keeping local listY-7 and clipping parent
together. This moves row1 below the points and row8 above the footer without
changing row pitch or shrinking portraits. Live rendering still needs checking.

Previous DNF tests missed animation ordering. Native BattleTeam row52d0 plays
ListItemCenterIn/RightIn (6240/6628); their float tracks6420/68e0 have final
keys64d0/6990 of1, overwriting the earlier row opacity. Native RaceRanking has
separate NotFinished/Finished tracks, which BattleTeam lacks. Store DNF0.6 in
the row color alpha(+cc) instead, preserving the entrance opacity(+e0). Its
inherited color covers the full row. Restore alpha1 on rebinding a finisher.
Regression now models the entrance writing opacity1 after binding and checks
DNF alpha remains0.6; all eight identities and repeated bindings are covered.


## 2026-11-01 — native name fitting and actual DNF animation conflict

Screenshot221730 confirms color alpha did not fix DNF; previous correction
was insufficient. Native PlayerTagText export6e70 already has forceFitInBox
(+68)=1 and wordWrap(+69)=0. Keep the native font fitter and reduce name
placeable width259->185 at x158; points begin x359, leaving16 logical units
of gap. No character-count estimate, truncation or cumulative scaling.

Verified EXNM metafields: RaceRanking NotFinished float track6e28 field index2
and BattleTeam entrance tracks6420/68e0 field index0 both resolve to
igGuiPlaceable::_alpha. Native finish entrance has only position/scale tracks;
BattleTeam additionally fades _alpha to1, overriding runtime DNF. Remove just
those two entrance alpha tracks from the generated BattleTeam asset, retaining
the position/scale tracks and all native keyframes. Restore runtime DNF writes
to row+e0 (native alpha), rather than the ineffective color alpha experiment.
The generated scoreboard keeps its movement/bounce entrance; its fade is
removed so status owns alpha, matching the native finish panel.

Asset tests use the real game dump and verify both retained track lists and
forceFitInBox/no-wrap, in addition to runtime USA/EU status/rebinding fixtures.
The game is not running; rendered output still requires live verification.


## 2026-11-01 — scoreboard-only team presentation and selector duration

`CNetworkConfig::_privateLobbyStartMatchTimerDuration` is field +0x110,
name table 0x268fbb8 (index 0x41), initialized to 10.0 seconds in 0x196ae96
(metadata default slot +0x208). Native team selector threshold is +0x118,
default 5 seconds. Private Single teams now extend the host-owned initial
countdown to 60 seconds and open the selector immediately. The native -1 latch,
replication and all-ready fast-start remain owned by eb7950. Guests do not
replace the host's remaining time. Other race types retain their duration.

Presentation waits for both the scoreboard RaceFinished event and validated
host results; a race-scoped latch prevents replay on duplicate packets or UI
refresh. Use the native kart animation accessor (vtable +0x110), cb0c70 with
`VictoryLapAnimSelector` (0 win / 1 loss), then c64790 `OnVictoryLap`, matching
e20c30's Battle win/loss presentation. Do not modify racer finish flags,
positions, or call the achievement/reward routines f35470/f35730.

For a local team winner, f382f0 emits only CRacerWinRaceMessage to the world,
with the racer's kart at message +0x38. The native common_Race_Start_Event_Chain
graph handles that message and spawns `ctr_confetti_loop`. Individual finish
callbacks stay native. The losing team gets the native loss animation.

The common_Race_Music_Manager graph branches its finish sting via hasWon_VSC:
EXID 11 (0x8305d2d3 / common_bank) is UI_Player_Lap_Change_End_Win;
EXID 12 (0x789c92ee / common_bank) is UI_Player_Lap_Change_End_Lose.
Play that same named sound through 1024340, the underlying playSoundByName_v2
routine. It searches the entity bank then loaded global banks. Pass an empty
native callback context and release the returned sound object as the VSC
wrapper does; playback remains native. Log sound resolution alongside the
team result to distinguish a missing asset from a presentation timing issue.

Validation: USA/EU runtime fixtures cover host/guest countdown, -1 latch and
ready-shortened remaining time, no effects before host results, native winning
animation/confetti/sting, native losing animation/sting without confetti, and
no repeat effects on duplicate results. The existing 17 asset tests pass.
Address/undefined sanitizer checks run with function-type checking disabled
for the online fixture: that check reads metadata before JIT trampoline
addresses, which have no compiler type metadata. Visual/audio playback still
requires a live team race; mock dispatch tests do not prove rendering.


## 2026-11-01 — private Start gate and team selector identity

The two-player/four-team bypass used the wrong No AI source: the native
Difficulty selector (125fb30) reads active race settings at 2ad4e30 +0x10,
whereas the old gate read the private configuration copy +0x18. Validate the
active flag and team count (+0x28), with the human list at player manager
2ad1160 -> +0x10 -> count +0x0c. Two teams permit 2/4/6/8, four teams permit
4/8. AI-enabled, public and Battle modes preserve native handling.

The update.pak privategame_buttonready graph calls canStartGame_VSC before
setNetworkCanBeJoined, lobbyStartGame and the navigation lock event. Mark only
that canStart node with one unused null input pin; native canStart has no
inputs. The wrapper at 12108f0 rejects that marked branch before the graph
locks the lobby. Ordinary unmarked rendering queries still call native
f507f0 and follow their original true/false output. Also guard the actual
lobby Start broadcaster ebe570 before it sends the event or sets self+0x42.
Its trampoline relocates the RIP-relative stack-guard load. USA/EU hook
addresses differ by 0x10; the stack-guard global remains 2807098.

The warning uses a548e0(text, NULL), the same native dialog called by a61ec0
when Online entry fails (including the offline branch at a62278). It states
the allowed count, current count and the option to enable AI.

CGuiBehaviorOctaneTeamSelectPlayerSlot has name at +0x10 and PlayerNumberText
at +0x18. Its update vtable entry 2544140 points to 9c60b0. After native update,
hide the local PLAYER n label and move name Y from 85 to the label's Y=60,
marking the transform dirty. Reapply on updates because native binding can
restore visibility. Keep native local slot numbering internally; no network
identity changes. Apply only in private/active private team selection.

Validation: USA/EU runtime tests cover both the marked pre-Start branch and
broadcast guard across public/private, 1/2/4 teams, AI on/off and 0–9 humans,
with deliberately contradictory stale private AI settings. Unmarked queries
and native true/false routing remain intact. Repeated slot updates retain
hidden labels and raised names; non-private behavior stays native. All 18
asset tests pass against the USA game dump, including the new Start marker.
ASan/UBSan (except generated-code function metadata), archive preparation and
CPU executable build pass. Live two-client selector/popup display still needs
verification in the game; these tests do not establish rendered appearance.


## 2026-11-01 — countdown initialization and deferred team finish effects

User reported host 10 seconds / guest 60 seconds, followed by host race-start
failure and guest host-left notice. The previous eb7950 tick hook changed the
component's remaining time after eb71d0 had already created Octane_Lobby_Timer
with the original value. That UI receives its initial duration via 110d3b0
event +0x18. Changing component +0x28 in a later tick does not update that
already-created timer. This ordering mismatch was absent from the old mocks.

Hook eb71d0 (EU -0x10) before native initialization instead. For private Single
2/4 teams only, temporarily supply network->config (+0x40) duration +0x110 =
60 and selector threshold +0x118 = 60 while the native initializer runs, then
restore both config values. Native initialization chooses authority using
owner +0x3b bit 0: the authoritative component reads the duration; a peer
retains its replicated remaining value. Both then create their UI from that
component value. Tick no longer extends remaining time; it only preserves the
immediate selector threshold behavior until native -1 latches. Native ready
shortening and remaining-time replication stay untouched. This addresses the
observed timer mismatch; the two-client disconnect symptom still requires
live confirmation after the fix.

Hook f382f0 (EU -0x10), the CRacerWinRaceMessage emitter, to suppress ordinary
individual confetti throughout private Single team races. Its original body
is callable via a relocated trampoline only by the existing validated
scoreboard presentation. Rewards, finish flags, placing and completion
messages are not suppressed. Non-team races still dispatch the original.

In both update and update2 common_Race_Music_Manager graphs, mark exactly the
two playSound_v2 nodes for UI_Player_Lap_Change_End_Win and _Lose with a fifth
unused pin. Verify their EXID hashes 8305d2d3/789c92ee in bank 4305904b.
Hook 1093ca0 (EU -0x10) to skip only these marked sounds in private team races.
Other sound nodes retain native handling and graph continuations remain
intact. The scoreboard's named sound playback bypasses this VSC hook and
plays the team result once, alongside winner-only confetti. Delayed individual
callbacks cannot replay the suppressed effects after the scoreboard either.

Validation: USA/EU runtime fixtures cover host initialization before UI,
replicated guest initialization, subsequent ticks without resetting time,
ready-shortened time, non-team/public durations, suppression of individual
team confetti/stings, non-team and unrelated-sound passthrough, and existing
scoreboard winner/loser once-only effects. Asset checks validate actual native
prologues and that only the two exact stings are marked in both archives.
ASan/UBSan (excluding generated-code function metadata), archive preparation,
and the executable build pass. Live network transition and audiovisual
behavior remain unverified; fixtures do not establish two-client gameplay.


## 2026-11-01 — live two-client early-ready failure and gate state mutation

Inspected the running client processes 899014/899142 and their latest logs
(233559 / 233603). Both mapped the same built executable inode. Native timer
initialization was 60.0 on the host and 59.9 on the guest. Both opened the
four-team selector with two humans. Host logged map load after Ready; guest
did not. User confirmed loading begins immediately when everyone is ready.
This disproves the old 10/60 display mismatch as the remaining cause.

The overlooked native config field +0x114 is 10.0, copied to timer +0x30.
It emits CLobbyCustomizationLockedMessage (factory120f8a0/type2ae31b0), whose
handler ebc8a0 does more than lock UI: it sends private match data, prepares
racer/player state and calls the network platform lifecycle. With duration
60 but this threshold left10, that preparation was postponed50 seconds.
The all-ready path9c2340 can call ec0e50 to load the host's map immediately.
The duration override now includes +0x114, preserving the native relationship
that preparation happens at the start of private countdown, before early
Ready can start loading. All three config fields are restored afterward.
Tests assert host and replicated guest have preparation threshold60, retain
normal/public configuration, and do not extend a ready-shortened timer.

The blocked-Start graph previously checked canStartGame only AFTER
readyPlayerLobby. That changed host ready state and locked/hidden menu controls
even though the later guard stopped the broadcast. Insert a host-only native
isHost2 branch BEFORE readyPlayerLobby instead. Its marked canStart wrapper
performs only validation, continuing to native ready when valid; it must not
ask native canStart yet because the host is intentionally not ready. Guests
continue to native ready without the host gate. The original later canStart
query is unchanged. Register RNEX/RPID/RVTB/ROFS relocation sites for both
new action nodes and output lists. Runtime regression covers repeated denied
clicks; asset regression proves the guard precedes ready mutation and that
native action nodes stay unchanged.

Build prepared separately while clients remained running; replacing the
executable pathname does not update either already-running process. Both
clients need relaunch for the new runtime and prepared graphs. GUI capture
through Orca was unavailable because its Python cannot import AT-SPI/gi;
inspection used process mappings, native memory and logs. The new complete
ready/start flow and modal appearance still require a live retest.


## 2026-11-01 — team selector duration reduced to 30 seconds

User requested 30 seconds instead of 60. Set private team initialization
config +0x110/+0x114/+0x118 together to 30, preserving first-tick peer
preparation, replicated guest time and native all-ready start. Updated the
existing USA/EU runtime assertions; runtime tests and executable build pass.
No new live two-client test for this duration-only change.

## 2026-11-02 — private team selector Back legend

Mark only the Back (enum 2) addButtonToLegend node in update.pak's
Octane_TeamSelect root with a fifth null pin. Hook 110bbc0 (EU -10), copying
its verified 17-byte prologue, suppresses that marked legend in private Single
team selectors (Normal/Itemless). Other legends and native Battle still call
the original wrapper. Ready/unready input behavior is unchanged.

USA/EU asset tests verify exactly one marked node, preserved original inputs
and executable prologue. Runtime tests check private Back suppression, Ready
forwarding, and Battle Back forwarding. User confirmed earlier confetti/audio
and 30-second countdown changes work. New legend requires relaunch; no live
visual verification performed for this change.

## 2026-11-02 — center the single name in the selector label area

Copying PlayerNumberText Y put the remaining name on the former upper line.
The adapter now centers the name's rectangle inside the union of the original
name and number rectangles, using current height and pivot values. The authored
name Y is retained per slot so repeated updates do not progressively move it.
The common parent applies native scale/animation to both labels. Name X and
native text alignment remain unchanged. Restrict the adapter to private Single
team selection; Battle and CUP retain native labels.

Runtime tests cover normal binding resets, twenty updates without a native
position reset, a label-height change, and leaving private mode. USA/EU runtime
tests pass. Visual centering still needs confirmation in the running game after
relaunching the rebuilt executable.


## 2026-11-02 — winner heading must display the winner's points

The reported four-team screenshot shows GREEN TEAM WINS but 7 POINTS even
though Green's row awards are 10+4=14. Aggregation already uses the current
10/8/6/5/4/3/2/1 awards (superseding historical 9/6/3/1 notes). The heading
incorrectly selected the local team's total: Blue's 6+1=7 in that capture.
The points heading now always selects team_winner, matching its adjacent name;
the WINS/LOSE label still reflects the local team. No award or tie-break changes.
Tests cover the reported eight-racer arrangement from every local player's
perspective, unchanged row awards, two-team losers and tied leaders. The old
runtime fails the updated losing-view assertion. Screenshot evidence was removed;
no replacement screenshot is claimed before a fresh live capture.
