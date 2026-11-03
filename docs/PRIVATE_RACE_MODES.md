# Private race modes

Private Single Race and Cup support Normal and Itemless. Itemless remains active
through lobby transitions and across Cup races.

| Feature | Private Single | Private Cup |
| --- | --- | --- |
| Normal / Itemless | Supported | Supported |
| Team Race | Supported | Not supported |
| Modified AI difficulty | Supported | Supported |

## Compatibility

Private Itemless, Cup, Team Race, and modified AI difficulty check compatibility
before starting. A native notice names incompatible players, and everyone stays
in the lobby. Confirming the notice only closes it. Checks also cover roster
changes during team selection.

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
