# Optional PSN backend and local notification relay

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
The optional production bridge adapter now connects this host binding to a
versioned authenticated loopback relay. It keeps the provider's semantic mapping
outside OCTR and forwards the local publisher capability instead of fabricating
the legacy port's token. End-to-end game acceptance remains separate work. No
Steamworks SDK files or provider implementation are included.

The existing Discord join file could not provide this integration: it describes
a nickname and session but not a platform account namespace, subscriptions or
live friend changes. A host extension is therefore necessary for actual PSN
callback delivery. The adapter uses the ordinary PSN imports rather than game
memory hooks. It preserves the original default path when not selected.

The relay's independent event worker avoids blocking friends/invitations behind
a slow session request. A replay cursor acknowledges copied events and prevents
a repeated HTTP response from duplicating an accepted invitation. Queue loss,
account replacement or malformed messages fail the selected session. Identity
is mapped only to local user 1, and unsupported authorization-code and invitation
UI operations remain explicit failures. See the [backend contract and build
instructions](../../platform/psn/backends/README.md).

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

The 2026-10-20 pass repeated the full Windows build and its 25 existing module
tests, the 12 focused launcher tests, and Python discovery with the same four
environment-dependent failures. The optional backend adds an HTTP fixture using
the real module and real guest callback adapter: nine scenarios cover transport,
request ownership/cancellation, a six-second response, filter fanout, callback
unregistration, exact sender identity, accepted-invitation replay, malformed
messages, stream overflow and account change. The combined standalone CTest run
passes 26 tests. The fixture runs only on local loopback and uses synthetic data;
it does not run the game or any platform client.
