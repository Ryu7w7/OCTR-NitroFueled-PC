# Public Itemless matchmaking

Public Itemless uses a separate queue from Normal. Party members follow the
confirmed lobby mode. Both the party leader and members see
`SINGLE RACE - ITEMLESS` or `SINGLE RACE - NORMAL` on the map panel.

## Opponents

Public Matchmaking supports parties of up to eight. A full eight-member party can form a
room immediately; solo players and parties of two through seven must find a non-overlapping
outside search. Ranked always requires an outside search and limits each party to four.
Two compatible solo players suffice; filling all eight seats is not required. Private is unchanged.

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

Live diagnostic evidence located the teardown callback; the corrected graph paths have
USA/EU asset and C regression coverage. Two-client testing confirms Waiting for Host,
repeated Private entry/exit, Ranked/Public switching, and Private-to-Public/Ranked re-entry.
Party retention at the 180-second timeout still requires live retesting.

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

The main lobby has a Ranked button below Matchmaking. Ranked offers Race Normal
and Race Itemless; Battle is disabled. It uses the `octr_ranked` version 1 search
extension and a server-owned queue filter, so neither Ranked mode joins a casual
room. Before sending the search, the client checks the signed
`/ryunet/ranked-party` endpoint. An older server cannot silently place a Ranked
search in a casual queue. The client verifies `octr_ranked_v1` in the backend
lobby document before accepting a room. At the validated results event every
client's mod posts the finish order to `/ryunet/ranked-result`
(docs/RANKED_SYSTEM_ARCHITECTURE_SPEC.md): the server scores the race when the
room's host reports and the lobby card shows the rating it answered with.

## Elapsed matchmaking

Public and Ranked show a native-style elapsed search timer, capped at 180 seconds.
At the cap a dedicated native UI route cancels the search without quitting the party or
reloading the scene. It returns the leader to the previous Ranked/Matchmaking selector,
retaining the selected mode; guests retain their party and the native party waiting route.
Ranked also shows server-owned range expansion; Public has no rank-range message.
Followers wait for the host's native party-return notification rather than dispatching their
own timeout Back. The previous independent timeout path split parties in two-client live
testing; the latest correction has regression coverage but still needs live retesting.
A confirmed match hides elapsed time and retains the native pre-race countdown.
The timer artwork is prepared once through the existing archive cache.

Ranked parties and active accounts per device are each capped at four. No IP restriction
or localhost bypass applies. The party limit applies even when every member uses a different
device. Public Matchmaking does not use these Ranked admission limits. A party of five or
more is blocked at Ranked entry with a native host notice. If a fifth member joins during
Ranked search or its confirmed lobby, the client cancels Ranked and notifies the host while
keeping the party together. See the Ranked architecture spec for device identity limits.

## Implementation status

Implemented in the current branch: queue separation, room confirmation, device-based Ranked
admission, elapsed timer, server range messages, native timeout cancellation, and cached timer
assets. See [Ranked roadmap](RANKED_ROADMAP.md) for shipped features and remaining work.

The latest timer/device changes have automated coverage but still require live multi-client
validation. Their implementation does not imply production server deployment.

## Verification

I tested public matchmaking on a local server with two clients: Itemless matched
with Itemless, and Normal did not match with Itemless. I also tested the public
Itemless compatibility gate with an outdated client.

Automated tests cover queue-mode confirmation, party compatibility, lobby labels, device
serialization, party growth, elapsed/range display, stale status replies, and timeout.
USA/EU native asset and widescreen checks cover the generated timer panel. Eight-client gameplay has not been verified. Automated coverage does not
establish full multiplayer behavior for every party, rematch, or host-change
combination.
