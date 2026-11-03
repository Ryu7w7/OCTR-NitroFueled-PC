# Private Team Race

Team Race supports private Single Race in Normal and Itemless, with two or four
teams. Teams are disabled in Cup and public matchmaking.

## Selection and compatibility

All participants need a compatible patch before starting. A native notice names
incompatible players; confirming it closes the notice without starting the race.
Compatibility checks also cover roster changes during team selection.

The selector shows READY while unready and UNREADY while ready, with the matching
confirm/cancel button. Only the current action is shown. The Lobby Members
overlay is hidden during selection and restored when leaving the selector, so
its roster cannot cover the team slots.

## Points and results

Team Itemless requires completed laps and a valid final time. A racer who does
not finish receives zero points, even if native race teardown marks that racer
finished. Zero points do not increase the team total. Team Normal retains its
existing placement awards.

Repeated result updates recompute totals without accumulating points. If nobody
finishes an Itemless race, the panel shows `NO FINISHERS` and zero points.

The team result decides the native victory/defeat animation, sound, and confetti:

- A racer on the winning team celebrates even after DNF.
- A racer on a losing team uses defeat effects without confetti, even after
  finishing first individually.

Effects target the native local-player identity and play once per result.

## Finish panels and scoreboard

Rows retain their native backgrounds. Only position numbers use team colors.
Finished local racers retain the bright native highlight; their names, engine
icons, and POINT text use the native finish-time text color. Other rows use white
text and engine icons.

DNF rows have no bright highlight. The entire row fades, including position,
portrait, engine icon, name, background, and points. POINT text keeps a white base
color and inherits the row fade.

Additional Team Race portrait/engine icons stay hidden in Battle scoreboards.
Team markers reuse the native Battle arrow: AI shows only a colored arrow;
human players have a white name below the arrow.

## Verification

I tested the private Team Race compatibility gate with an outdated client.

Automated USA/EU tests cover team totals, Itemless DNF scoring, repeated result
updates, finish/scoreboard colors, DNF fading, Battle icon isolation, and selector
overlay visibility. Effect tests cover a winning DNF racer, a losing first-place
racer, local-player lookup, and duplicate-result suppression.

The latest highlight colors, Lobby Members hiding, and local-player effect
changes still need in-game verification. Automated tests do not establish full
multiplayer coverage for every team size, rematch, or host-change combination.

See [private race modes](PRIVATE_RACE_MODES.md) for the overall mode restrictions.
