# Public Itemless matchmaking

Public Itemless uses a separate queue from Normal. Party members follow the
confirmed lobby mode. Both the party leader and members see
`SINGLE RACE - ITEMLESS` or `SINGLE RACE - NORMAL` on the map panel.

## Opponents

Public Matchmaking supports parties of up to eight. A full eight-member party can form a
room immediately; solo players and parties of two through seven must find a non-overlapping
outside search. Ranked always requires an outside search and limits each party to four.
Two compatible solo players suffice; filling all eight seats is not required. Private is unchanged.

Public Itemless disables player/AI kart collisions and places every racer on one
starting row within the native front-row width. Track and wall collisions remain
enabled. Ranked Itemless uses the same collision and starting-position policy,
with its own fixed five-lap, human-only race rules.

## Parties and compatibility

The server validates every party member as part of the native asynchronous
matchmaking request. Go To Lobby does not wait for a separate HTTP preflight or
its cache. Incompatible parties are rejected through the native matchmaking task;
rejection does not fall back to Normal. The client still verifies the server's
Itemless lobby confirmation before enabling Itemless gameplay.

## Party lifetime during menu navigation

Back first opens the native confirmation dialog without changing party/search state.
Cancel preserves the search, elapsed time, and party. Cancellation is committed only when
the accepted exit returns focus to its destination menu (or the timeout explicitly fires): cancel the queue, leave/reset the race session with party retention, finish
closing the party's race lobby without restarting search, then queue native Quit with Party
notification for followers. Selected mode and Ranked/Public context remain unchanged.
In menus, the guest uses a dedicated host-closed signal and the native host/guest branch,
which registers `Octane_Lobby_WaitingRoom` through native `setLobbyMenuName_VSC` and
opens Waiting for Host with Customization only instead of Mode Select;
if that matchmaking GUI is absent, it explicitly loads Online Lobby. It does not use the
race-exit handler that can load Start Screen. During a frozen race, native handling remains.
The domain archive uses the same modern matchmaking graph/UI pair as update2; its older
graph lacks the native Waiting Room route. A later host return does not reload Online Lobby
when the guest has already returned and has no active matchmaking GUI or private lobby.
A new lobby lifecycle or private lobby still permits the fallback. Registering Waiting Room
is required because native `CGuiEventEnterLobby` targets the registered menu name, not the
topmost project; retaining the old matchmaking name prevents followers from entering again.
Private lobby focus also rearms the return lifecycle. Its GUI project receives native
`CGuiEventPrivateLobbyClose`, rather than a matchmaking input or full scene reload.
The 24-byte Private event has no input signal/controller fields. Before dispatching it,
the client also updates the registered menu name to Waiting Room through the existing
native string helper. Otherwise Private closes visually but the next Public/Ranked entry
still targets the closed Private project.

Lobby graphs replace `stopMatchMaking_VSC` and Private's `disconnectAll_VSC` with race-session
reset. The original matchmaking stop schedules an asynchronous network-close callback that
resets the party; it cannot be used for party-preserving Back. Main's explicit offline-exit
graph is unchanged. Explicit Leave Party, kick, and disconnect still perform normal cleanup.

Search generations invalidate old replies, polling stays suspended until lobby re-entry,
and elapsed time resets. Duplicate lobby entries and cancelled-host callbacks are ignored.
PSN race-session cleanup retains a party while native transport connections still exist.

The corrected graph paths have USA/EU asset and C regression coverage. Waiting for Host, repeated Private entry/exit and Public/Ranked re-entry retain the party.
The client advertises `collision_rules: 1`; the server requires it from the search
and every party member. The server's `octr_itemless_v1` field uses confirmation
version 2 for these rules. Older confirmation versions are rejected, so the client
cannot silently race against older collision/grid rules. This requires the matching
server update; it does not create a second selectable Itemless queue.

## Mode lifetime

Itemless assets and gameplay follow the confirmed lobby identity. Leaving,
cancelling, disconnecting, or changing rooms clears the confirmation. Loading a
track and rematching retain the current mode.

Public Itemless uses valid final times when the host builds finish order, rather
than stale stored positions. Clients retain the validated host order for final
results even if their local times or positions differ. Malformed orders cannot
replace the last validated result, and loading the next map clears it.

Cup, Team Race, and modified AI difficulty remain unavailable in public
matchmaking. See [private race modes](PRIVATE_RACE_MODES.md) for those features.

## Ranked queue

Ranked Normal and Itemless use `octr_ranked` version 2 and a server-owned filter.
Members preflight through signed `/ryunet/ranked-party`; `octr_ranked_v1` room
confirmation must match mode and registered nonce. Registration preserves that nonce
across native reloads. Joining waits for pending native session work.
Ranked uses five laps and human-only fields; start minimums come from server ENV.

Every client submits original human identity, finish evidence and DC through
`/ryunet/ranked-result`. The client handles the authoritative response without requiring a host-first report.
See [Ranked specification](RANKED_SYSTEM_ARCHITECTURE_SPEC.md).

## Elapsed matchmaking

Ranked shows elapsed time up to 180 seconds and server-owned range expansion.
The native timeout path preserves the party and selected mode. Followers wait for
the host's return notification. Public suppresses the client Ranked timeout/range
route; reservation expiry remains server-owned. Confirmed rooms
use the native countdown. Timer assets reuse the archive preparation cache.

## Room and party lifecycle

Room departure and premade departure are separate. Old returns, updates and
cancellations cannot replace a new search. Public followers clear stale Private
state. Post-race room notifications preserve the premade party.
A premade alone waits for an outside search. Solo survivors retain room chat and
waiting context; actual exits release native peers, roster and countdown.

Ranked parties and active accounts per device are capped at four, without an IP
restriction or localhost bypass. Growing beyond four cancels Ranked while retaining
the party. Public does not use Ranked admission limits.

Host migration uses native reconnection and ownership binding with race-ID
preservation. See [migration architecture](HOST_MIGRATION_ARCHITECTURE.md).

## Verification

Regression tests cover queue-mode confirmation, party compatibility, device
serialization, party growth, stale replies, timeout, USA/EU hooks, native assets,
race transitions and host/guest departure with 2–8 racers.
