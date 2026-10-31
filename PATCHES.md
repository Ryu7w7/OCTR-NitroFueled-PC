# Private CUP, Graphics settings, and Itemless

Updated 2026-10-31. This document describes the current changes and validation,
not the superseded development candidates.

## Private CUP races

- Adds an independent **RACE TYPE: SINGLE / CUP** selector to private racing
  settings. Public matchmaking and Battle keep their existing flow.
- Reuses the native picker for the 11 non-Gem, four-track CUPs, including their
  original track order, names, preview materials, and host/guest lobby cards.
- Advances both clients through four races using the native next-map path and
  countdown. Keeps the AI roster and player identities across map teardown.
- Carries cumulative points between races and accepts authoritative host results
  without double-counting repeated or late finish events. Uses native 9/6/3/1
  awards and the previous race's finish positions for subsequent starting grids.
- Keeps the normal local-finish time panel alongside CUP standings.
- Builds the final podium from cumulative CUP standings using the native sorter
  and tie rules, while native online code retains names and customizations.
- Skips stale intermediate podium data when a host disconnects before final
  results arrive. It does not invent an outcome for an incomplete CUP.

See [native CUP integration](docs/NATIVE_PRIVATE_CUP_RESEARCH.md) for ownership,
transition, scoring, and executable-contract details.

## Graphics settings

- Uses native setting selectors, readable titles/values, and Gameplay-style
  spacing instead of shrinking controls to fit the whole list. The rebased
  version retains upstream Aspect Ratio choices (16:9, 21:9, 32:9); supplied
  screenshots predate that upstream label change.
- Uses **RESTART TO APPLY** with the native checkmark material. The restart
  action is highlighted only when focused, not merely because changes exist.
- Places white, horizontal help text at the lower left outside the blue panel,
  with viewport-aware safe padding; removes the old separator line.
- Positions Graphics after the last available native Options entry for each
  layout, including in-race layouts without Extras/Hints, and preserves the
  following Privacy entry's spacing.
- Supplies the widget/material dependency closure outside Online menus and
  preserves all copied animation relocations, including RNEX metadata. This
  addresses the Options-opening crash caused by an unresolved animation field.
- Selector-background clipping at the top of the scrolling list was still
  reported in the last visual check; it is not claimed fully fixed here.

## Itemless

- Renames the visible Speed Mode choice and launch messages to **ITEMLESS**.
  Existing internal Speed identifiers and the RyuNET wire value remain compatible.
- Preserves boost reserves during player/AI racer contact in active private
  Itemless races. Native contact response still runs.
- Limits suppression to the matching boost owner during racer contact; normal
  mode and wall/obstacle contacts retain native reserve behavior.

## Validation

I tested the multiplayer changes myself with host and guest clients on Linux.
I confirmed CUP lobby display, multi-track progression with AI, scoring, the
local-finish panel, subsequent grid placement, and the corrected final podium.
I also confirmed that hitting another player or AI in Itemless does not clear
reserves. The supplied Graphics screenshots show the in-race entry placement
and selector layout; the unfocused restart button was separately confirmed.

Automated coverage includes:

- C adapter tests for both regional layouts: private/public/Battle routing,
  four-track lifetime, roster/identity restoration, authoritative and repeated
  results, native podium integration, and incomplete-CUP lobby return.
- Itemless contact tests for humans/AI, ordinary mode, non-racer contacts,
  owner scoping, and nested callbacks.
- Graphics runtime tests for settings/navigation/layout behavior, plus
  synthetic and real USA/EU hook installation and rejection checks.
- Local USA CUSA13795 and EU CUSA14876 **1.21** executable/asset checks: native
  CUP inventory, menu dependencies/relocations, hook contracts, and execution
  of the original CUP sorter with isolated external-call substitutes.
- Linux runtime compilation with warnings treated as errors, linking the
  available packaged GPU library; this is not a fresh full renderer build.

Asset checks on both regions are not a claim of a live mixed-region session.
Windows gameplay and exhaustive ultrawide/display configurations have not been
validated. Screenshots are published separately on the fork's evidence branch;
the PR links each image and distinguishes race awards from cumulative totals.

### Reproducing targeted checks

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -Isrc tests/test_online_mode.c src/runtime_online_mode.c -o /tmp/online-mode-test
/tmp/online-mode-test
cc -std=c11 -O2 -Wall -Wextra -Werror -Isrc tests/test_itemless.c -o /tmp/itemless-test
/tmp/itemless-test
c++ -std=c++20 -O2 -Wall -Wextra -Werror -Igpu/shim -Igpu/shadps4 tests/test_graphics_tab.cpp -o /tmp/graphics-tab-test
/tmp/graphics-tab-test
# Repeat for each supported local 1.21 dump.
export PYTHONPATH=scripts BB_TEST_GAME=/path/to/game
for suite in online_mode native_cups graphics_assets graphics_tab itemless_native; do
    python3 -m unittest discover -s tests -p "test_${suite}.py" || exit 1
done
```

The five Python suites pass **21 tests per region (42 total)**. Graphics C++
checks require permission to read the process's own memory on Linux.
