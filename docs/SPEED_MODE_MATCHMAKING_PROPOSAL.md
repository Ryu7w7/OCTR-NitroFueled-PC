# Proposal: Speed Mode matchmaking for RyuNET

Hi Ryu, I would like to propose a separate public **Speed Mode** race queue, with its own MMR if ranked play is added later. I have tested Speed Mode in a private match with another player, including a match without AI. The current implementation is experimental and requires every participant to use a compatible SacredFire build with Speed Mode enabled.

## Rules players should see

- The launcher offers **Speed Mode (Experimental)**. Players who choose public matchmaking with it enabled are matched only with other Speed Mode players. Normal Race and Battle matchmaking remain separate.
- Speed races have no item boxes, Wumpa boxes, item or Wumpa HUD, or seasonal ghost/Golden Wumpa pickups. Shortcuts that normally require an item should be usable without one.
- Adventure and Arcade keep their normal boxes and HUD. Private Speed matches continue to work independently of the public queue.
- An incompatible client cannot join a Speed lobby, and a Speed client cannot enter a normal public race with Speed assets active.

## Suggested server/client contract

These message names are proposals; they should be adapted to the matchmaking protocol RyuNET already uses.

1. The server advertises a versioned Speed matchmaking capability, for example `speed_matchmaking: 1` in `/ryunet/theme`. Until it does, public matchmaking keeps normal race assets even when the private Speed switch is enabled.
2. When a player starts **public matchmaking**, the client sends `race_mode=speed` and its Speed content version in the authenticated matchmaking request. The server puts the player in a separate Speed queue. A login-time preference alone is insufficient because a player can enter and leave different game modes during one session.
3. Before loading a public race, the server confirms the lobby's race mode and compatible content version to every participant. The client activates Speed assets only for a confirmed Speed lobby or a private Speed room. If mode confirmation fails or conflicts, the client cancels the join instead of entering a mixed-rule race.
4. The server enforces mode separation for lobby creation, joining, migration, and rematches. It should reject incompatible clients rather than silently falling back to the normal queue.

## MMR, after queue separation works

- Keep Speed MMR separate from normal Race and Battle ratings.
- Calculate it on the server from authenticated **completed public Speed races**. Private matches, AI races, and abandoned sessions should not grant rating.
- Define handling for disconnects, ties, parties, and small lobbies before enabling rating changes. An unranked Speed queue can launch first.
- Return rating and queue status through a server API for the Linux and Windows launchers to display. The client should never submit its own authoritative MMR.

## Current client state and remaining work

The current Speed build generates normal and Speed archive views from the player's own game files (`scripts/speed_mode.py`) at launch. `src/runtime_file.c` selects the Speed view when the in-game switch is on and a private WebAPI room has recent game traffic with another player (`src/runtime_services.c`, `src/runtime_net.c`). The in-game switch applies without restarting. Public matchmaking uses the normal view until the server supports a separate Speed queue. The launcher and in-game menu expose the experimental switch.

For public matchmaking, the client still needs a signal for the **server-confirmed public lobby mode** and a compatible-build check. Seasonal pickup suppression also needs to follow the race mode; the present experimental patch suppresses it for the whole launch when Speed Mode is enabled. The private match tests do not establish that public queue isolation or MMR works yet.

Suggested first acceptance test: one Speed client and one normal client search at the same time and never enter the same lobby; two compatible Speed clients match, race with no item/Wumpa boxes, finish, then return to Adventure/Arcade with normal boxes present.
