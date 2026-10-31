# Native private CUP integration

Updated 2026-10-31. Private CUP progression and final podium behavior have been
confirmed in manual host/guest play. This replaces the investigation diary;
older candidates and their pending-validation statements are superseded.

## Scope and native data

The integration supports CUSA13795 (USA) and CUSA14876 (EU), version 1.21.
Native assets and instruction signatures are checked before patching. Regional
addresses must be verified individually: some routines differ by 0x10, others
have identical addresses, and metadata tables can differ by 0x40.

`loosedata/systemdata.igz` in `update2.pak` owns the CUP inventory and awards.
`CSystemData + 0x180` references `CCupPositionPointsData`: **9, 6, 3, 1**.
`CSystemData + 0x188` references `COctaneCupSelectDataListList`. CUP entries have
a name at +0x28, Gem flag at +0x38, count at +0x0c, and items at +0x20;
individual track paths are at +0x10.

There are 11 non-Gem CUPs and five Gem CUPs, each containing four tracks.
Private races use the native non-Gem list: Wumpa, Nitro, Crystal, Crash, Velo,
Aku, Uka, Lost, Bonus, Desert, and Space. No second playlist or scoring table
is maintained. Inspect a legally supplied local dump without modifying it:

```sh
python3 scripts/inspect_native_cups.py /path/to/game
PYTHONPATH=scripts BB_TEST_GAME=/path/to/game python3 -m unittest discover -s tests -p test_native_cups.py
```

## Private selection, assets, and lobby synchronization

`RACE TYPE: SINGLE / CUP` is independent of `GAME MODE`. Its cloned selector
preserves native timers, animation categories, ownership, and relocations. It
is hidden in Battle/public menus, with controller navigation adjusted around it.
The underlying private-race enum remains Single Race (1); CUP is native mode 6,
not Relic Race (3).

The original `Octane_CupSelect` project opens from private settings. The entry
route temporarily uses mode 6; accept/Back restore the prior mode and return to
private settings. Extra VSC pins identify these routes without changing the
original eight menu-open pins. Acceptance publishes the selected first map
through the existing native settings publisher.

Online streaming includes the CUP dependency closure from the Arcade/start
package: language files, preview materials, textures, and navigation graphs.
Files owned solely by the unloaded Arcade archive are supplied through the
existing loose-file map. Native references retain their string/handle types;
CUP data +0x78 is a string, not an igHandle.

The private settings event conveys CUP mode and the first map. The receiver
resolves the non-Gem CUP and creates its four-zone native handle list, while
keeping lobby settings in Single Race mode. Unknown maps fall back safely.
The host/guest postcard uses native CUP title, four-track preview, and mode
subtitle. A tagged post-binding update prevents later native GUI bindings from
replacing it with a Single Race card.

## Race lifetime and progression

The map-load boundary selects mode 6 and the appropriate CUP index. Active
series state survives disposal of lobby UI components. Native manager fields
include race mode +0xd40, CUP index +0xd68, and zone list +0xd80.

For tracks 1–3, the host's normal end-race timer advances through the native
next-CUP-map function (USA 0x1046200, EU -0x10). This function requires the
resolved current zone argument; the index getter is not a zero-argument API.
The timer caption shows NEXT RACE 2/4 through NEXT RACE 4/4. Track four uses
the native lobby-return path.

Online teardown recreates racers. Human/AI identities, cumulative points,
and previous finish positions are restored by identity rather than list order.
AI resources are loaded through native streaming before recreation; stale
entities/resource pointers are never retained across map teardown. Subsequent
grids read the previous finish position (+0x60), not cumulative CUP points.
First-track placement remains native.

The normal online countdown HUD is enabled for active private CUP. A native
`Octane_Race_Ranking` project supplies the local-finish time panel alongside
`Octane_CupRace_Ranking`; full race-finished flow stays with the CUP project.
Project handles are released before teardown, including repeated/late events.

## Authoritative scores and final podium

Racer +0xb4 holds race points and +0xb8 holds CUP points. Host results replace
provisional scoring, using the pre-race totals once per race; duplicate packets
and late native finish callbacks cannot award the same points twice.

The ordinary online result consumer (0xa298c0) stores podium data from the
last race's finish positions. That is incorrect for CUP: a final-race winner
can still be second in cumulative points. The reported reproduction had Hasty
25, n0buholic 21, Coco 12, and Liz 9; last-track order incorrectly put
n0buholic first.

For the final CUP result only, the integration sorts a copy of racer pointers
using the original CUP sorter (USA 0xeac6c0, EU -0x10) and comparator
(USA 0xeacd00, EU -0x10). Native cumulative-point and human/finish tie rules
are preserved. Only the podium slot is substituted: the original online
consumer still constructs names, drivers, and customizations, and per-race
positions/awards remain the actual last-track results. Temporary selection is
thread-local and cleared after the result consumer.

The podium branch at 0xa29a92 has only 13 bytes before a shared loop label.
Its stub preserves that label and redirects both incoming branches. Instruction
contracts and all three entry paths are regression-checked for both regions.

A separate completion flag records whether final authoritative results built
the podium. If the host leaves before that, lobby return clears only the native
pending-podium flag (lobby zone +0x1b0, byte +0x71) read by
`shouldShowOnlinePodium`. This prevents a track-three podium from masquerading
as the final CUP result. Completed CUPs and ordinary lobby loads keep their
native behavior. No final score is inferred from missing network results.

## Related Options crash

The in-race Options crash was caused by a copied Vec3 animation track missing
its RNEX relocation. Native setup treated unresolved metadata index 11 as a
pointer and faulted at 0x1504e30. Graphics asset generation now loads RNEX sites
before cloning, and regression checks compare every copied relocation kind,
including the metadata field at track +0x18. The fix applies to all native
Options layouts, including those without Extras/Hints.

## Verification and limits

I tested the CUP flow myself with host and guest clients, including lobby
cards, four-track progression, retained AI, cumulative standings, local-finish
UI, subsequent grid placement, and the corrected podium. I also confirmed
that Itemless racer collisions preserve reserves; that hook is separate from
CUP scoring.

Automated C tests cover both regional layouts, mixed human/AI order, resource
lifetime, repeated/late events, authoritative score correction, podium branch
continuations, and interrupted versus completed CUP returns. Python tests
check native data/assets and execute the actual native sorter on the reported
score totals and tie cases with isolated memmove/human-predicate substitutes.
Local executable and asset checks pass on both USA/EU 1.21 dumps.

These checks do not establish live mixed-region compatibility, Windows play,
or every disconnect timing. No game archives or executable dumps are committed.
For Graphics/Itemless behavior and the remaining selector-background clipping
limitation, see [PATCHES.md](../PATCHES.md).
