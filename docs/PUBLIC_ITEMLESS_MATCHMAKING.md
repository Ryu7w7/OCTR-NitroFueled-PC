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
lobby document before accepting a room. Ranked currently separates queues; MMR
and rating changes are not implemented.

## Verification

I tested public matchmaking on a local server with two clients: Itemless matched
with Itemless, and Normal did not match with Itemless. I also tested the public
Itemless compatibility gate with an outdated client.

Automated tests cover queue-mode confirmation, party compatibility, and lobby
labels. Eight-client gameplay has not been verified. Automated coverage does not
establish full multiplayer behavior for every party, rematch, or host-change
combination.
