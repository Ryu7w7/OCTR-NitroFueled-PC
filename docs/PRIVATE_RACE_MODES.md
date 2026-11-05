# Private race modes

Private Single Race and Cup support Normal and Itemless. Itemless remains active
through lobby transitions and across Cup races.

Before a private race starts, the host confirms its mode with RyuNET. Guests
confirm that host mode before loading track assets. A failed confirmation stops
start/loading instead of using a previous choice. The confirmed mode stays fixed
through the race and all four Cup tracks; returning to the lobby allows updates
and requires confirmation for the next race. This requires a working room-mode
endpoint, including for Normal private races.

Normal and Itemless races (private/public Single, private Teams, and Cup) sort
valid finishers by final time before the host publishes results. A stale
stored position cannot override a valid faster time. After receiving a validated
host order, clients retain that order despite late local updates. Cup points
continue to replace provisional awards once per race, including duplicate packets.
Late human/AI finish callbacks restore all three native position fields from the
validated host order. Cup carries the accepted totals and previous finish grid
into the next track, rather than copying mutable racer state. If the Cup end
timer expires before results arrive, progression waits for accepted results.

| Feature | Private Single | Private Cup |
| --- | --- | --- |
| Normal / Itemless | Supported | Supported |
| Team Race | Supported | Not supported |
| Modified AI difficulty | Supported | Supported |

## Compatibility

Private Itemless, Cup, Team Race, and modified AI difficulty check compatibility
before starting. A native notice names incompatible players, and everyone stays
in the lobby. Confirming the notice only closes it. Checks also cover roster
changes during team selection. If a check is still running, one Ready/Start press
waits without blocking the game thread and continues once that same party and
feature set is verified. Navigation, loss of focus, a changed party/mode, leaving
the session, or a map transition cancels the pending action. Failed checks and a
15-second timeout show a notice; pending checks do not ask the player to retry.

## Related features

- [Team Race](TEAM_RACE.md): team selection, points, DNF, presentation, and effects.
- [AI difficulty](AI_DIFFICULTY.md): supported modes and compatibility requirements.
- [Public Itemless matchmaking](PUBLIC_ITEMLESS_MATCHMAKING.md): queue separation,
  parties, and public compatibility checks.

## Verification

I tested the private Itemless, Cup, Team Race, and modified AI difficulty gates
with an outdated client. The screenshots show the incompatible nickname and a
single CONTINUE button.

Automated tests cover USA/EU native layouts, compatibility checks, and Cup
progression. Full multiplayer gameplay has not been verified for every Cup,
rematch, disconnect, or host-change combination. Feature-specific verification
is recorded in the linked documents.
