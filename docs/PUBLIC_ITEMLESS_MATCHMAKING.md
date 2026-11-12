# Public Itemless matchmaking

Public Itemless uses a separate queue from Normal. Party members follow the
confirmed lobby mode. Both the party leader and members see
`SINGLE RACE - ITEMLESS` or `SINGLE RACE - NORMAL` on the map panel.

## Parties and compatibility

The server validates every party member as part of the native asynchronous
matchmaking request. Go To Lobby does not wait for a separate HTTP preflight or
its cache. Incompatible parties are rejected through the native matchmaking task;
rejection does not fall back to Normal. The client still verifies the server's
Itemless lobby confirmation before enabling Itemless gameplay.

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
At the cap the client cancels matchmaking and returns through the native lobby path.
Ranked also shows server-owned range expansion; Public has no rank-range message.
A confirmed match hides elapsed time and retains the native pre-race countdown.
The timer artwork is prepared once through the existing archive cache.

Ranked parties and active accounts per device are each capped at four. No IP restriction
or localhost bypass applies. See the Ranked architecture spec for device identity limits.

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
