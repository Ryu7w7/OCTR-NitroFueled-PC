# Optional PSN backend: shared bbport host module

The existing OCTR implementation mixes platform identity and PSN service replies
with RyuNET-specific configuration. A different platform cannot safely replace
just its friends list: authentication, request ownership, push registrations,
accepted invitations and session reads must refer to the same selected backend.

`platform/psn/` puts those contracts in a self-contained C11 module, with a native
C backend ABI and a separate PS4 guest ABI. It has its own CMake build and tests,
no dependency on OCTR headers, and no dependency on Steam, a renderer or a game
dump. The same module can be integrated by other bbport descendants. OCTR's shim
only resolves NIDs to symbols and connects startup and SystemService. Launcher
and socket policy remain host-specific.

## Compatibility decision

Selection is explicit and fixed before guest startup. Without `BB_PSN_BACKEND`,
the existing services table, RyuNET nickname admission, Discord invitations and
network tunnel run as before. A selected backend must satisfy the versioned
social profile; loading or initialization failure does not select RyuNET instead.
Unsupported external operations fail explicitly instead of borrowing synthetic
responses from another identity namespace. Local trophy services stay with OCTR.

An invitation accepted in the platform UI enters the SystemService queue. The
game then performs its original session WebAPI requests. A WebAPI invitation
notification alone does not imply acceptance. Likewise, opening a provider's
invitation UI does not imply a successful send: the dialog result carries only
acknowledged recipients. Friends/presence changes use registered PSN events;
there are no game-memory edits or new game-facing join/refresh commands.

The module implements the documented subset listed in its [README](../../platform/psn/README.md).
It is not a claim that every PSN import or historical SDK behavior is implemented.
In particular, a production backend and end-to-end game acceptance are separate
work. The first integration intentionally contains no Steamworks SDK files and
does not couple the publisher or transport to this library.

## Evidence and ownership

Layouts, callback forms and service separation were compared with public PS4
compatibility implementations, including shadPS4's `np_manager`, `np_auth`,
`np_web_api` and `invitation_dialog` libraries, and OCTR's existing invitation
event path. These are compatibility references, not official Sony SDK
documentation. Unknown callback signatures are not inferred from placeholder
declarations. New adapter code was written for this module; no proprietary SDK
source or game-derived files are included.

The host retains local user IDs, guest request/context handles, buffers and callback
registrations. Provider inputs are copied and provider outputs are copied before
the next call. The backend never receives a guest pointer. Host registry locks
may nest the backend call lock; backend event delivery releases that lock before
entering a registry, and guest callbacks run outside both locks. Registration
generation/pin tracking prevents stale callbacks after unregister or context
deletion. Abort/delete release provider work before discarding guest outputs.

## Verification

The standalone tests exercise real dynamic-library loading, ABI/capability
rejection, initialization/close failures, copied event data, accepted-invitation
byte layout and bounded queues. Adapter tests cover pending/completed auth,
cancel/delete, binary WebAPI request/response ownership, HTTP versus transport
errors, incremental reads, service filters, callback reentrancy and concurrent
unregistration, and invitation-dialog outcomes. Launcher tests verify that an
explicit external selection bypasses RyuNET admission and rejects Discord join
mutations while the default path retains its existing behavior.

Windows validation used the existing MSYS2 CLANG64 toolchain, including the full
`bash build.sh --test` build of the runtime and native launcher. The module's 25
CTest cases and the 12 focused Python launcher tests passed.

The full Python discovery run executed 151 tests: 109 passed, 38 skipped and four
failed in this Windows environment. Two require symlink creation privilege; one
requires Linux Bash through WSL; one assumes Linux Vulkan driver paths. The
affected existing tests and their implementation files were not changed. They
are not recorded as passing. A Linux build/test run remains required before
merging upstream; no Linux/macOS runtime or in-game acceptance is claimed here.

No game or platform client was launched for these checks, and no installed
binaries, profiles, saves or databases were replaced.
