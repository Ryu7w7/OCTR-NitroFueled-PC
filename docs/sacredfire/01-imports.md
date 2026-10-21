# Imports

Measured on CUSA13795 1.21 (2026-10-09), prepare.py + link_modules.py with libc.prx, libSceFios2.prx
and libSceNpToolkit2.prx linked: 713 eboot imports, 473,760 relocations, 52 modules; 290 native
bindings; 730 imports left to the host runtime (618 from the eboot, the rest from the linked modules).

Resolution (src/runtime.c): every import is named `NID#library` by link_modules.py. Patterns in the
runtime name a library (`#libc`, `#libkernel`) or match any library (pthread_* names). Symbol-keyed
tables (`RUNTIME_LOOKUP`) use `out/import_names.txt`, generated from shadPS4's aerolib for this eboot
and loaded with `--import-names`; the compiled `src/import_names.inc` (bbport's table, from Bloodborne's imports) is the fallback.
Imports from libc/libSceLibcInternal are never served by the runtime: they bind to libc.prx.

`out/sacredfire out/boot-linked.bin --list-imports --import-names out/import_names.txt`: before the
CTR-specific work, 281 resolved in the runtime, 171 natively, 350 unresolved (100 of those are GPU
imports that bind at GPU init). The unresolved set that remains to be reached (not yet hit at
run time): libSceFont 41, the NpToolkit2 module's own Np* imports (NpTus 41, NpMatching2 35, NpWebApi
20, NpScore 18, NpTrophy 16, NpUtility 10, dialogs), libScePosix sockets and sched params,
sceSaveDataGetParam/LoadIcon.

Update (same day, after the start-up batches): 451 resolved in the runtime, 279 natively, 290
unresolved of which 100 are GPU imports bound at GPU init; the rest are untouched so far: libSceFont
39, the NpToolkit2 module's Np imports (NpTus 41, NpMatching2 18, NpWebApi 10, NpUtility 10, NpScore
10, NpTrophy 8, NpManager 8, SigninDialog 5, ...), libScePosix sockets and sched params 11. None of
them has been called through the license screens, menus, driver select and a race.
