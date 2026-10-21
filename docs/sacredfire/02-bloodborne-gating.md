# Bloodborne-specific code and how it is gated for CTR

> Historical (2026-10-08). The Bloodborne profile and the pieces marked *removed* below were
> taken out on 2026-10-09 (09-bloodborne-strip.md).

| piece | where | state for the ctr profile |
|---|---|---|
| game check (image hash, 1.09 messages) | scripts/game_check.py | profile table: bloodborne and ctr |
| patches (frame rate, resolution, effects) | scripts/patches.py, patches/Bloodborne.xml | run.sh skips them for ctr (no --patches) |
| render scaling / live resolution / FSR asset build | run.sh | skipped for ctr |
| content profile (SKU) | scripts/content_profile.py | generic (param.sfo user params); used as is |
| libGnm offset hooks (518 entries) | gpu/shim/bbport_gnm_hooks.cpp | off unless BB_GNM_CENSUS/OBSERVE=1; entries byte-checked, so inert on another image |
| guest GPU-memory hooks, heap sites, free check | gpu/shim/bbport_guest_hooks.cpp, bbport_heap_sites.cpp, bbport_free_check.cpp | BB_GUEST_IN_PLACE/BB_HEAP_SITES/BB_LABEL_TRAP only; byte-checked |
| temporal upscalers, object motion vectors | gpu/shim/bbport_settings.cpp | default off when BB_GAME_PROFILE is not bloodborne (BB_UPSCALER/BB_OBJECT_MOTION still override) |
| libc.prx malloc_stats probe (frame stats) | src/runtime_pad.c | only for the bloodborne profile |
| import name table | src/import_names.inc | fallback only; out/import_names.txt is generated per eboot |
| hardcoded scoped NIDs | src/runtime*.c | now NID#library patterns, valid for any eboot |

Not yet reviewed: the overlay's scene constants (gpu/shim/bbport_overlay.cpp), TAA, render-scale
proxies (SceneTargets::Eligible). They have not misbehaved on CTR so far; review if a visual defect
points at them.
