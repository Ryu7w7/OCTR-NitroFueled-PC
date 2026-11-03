# Public Itemless matchmaking

Public Itemless uses a separate queue from Normal. Party members follow the
confirmed lobby mode. Both the party leader and members see
`SINGLE RACE - ITEMLESS` or `SINGLE RACE - NORMAL` on the map panel.

## Parties and compatibility

The party compatibility check runs before Go To Lobby starts matchmaking.
If a member is incompatible, the leader sees a native notice naming that player,
and the party stays in place. Confirming the notice only closes it.
Itemless rejection does not fall back to Normal matchmaking.

## Mode lifetime

Itemless assets and gameplay follow the confirmed lobby identity. Leaving,
cancelling, disconnecting, or changing rooms clears the confirmation. Loading a
track and rematching retain the current mode.

Cup, Team Race, and modified AI difficulty remain unavailable in public
matchmaking. See [private race modes](PRIVATE_RACE_MODES.md) for those features.

## Verification

I tested public matchmaking on a local server with two clients: Itemless matched
with Itemless, and Normal did not match with Itemless. I also tested the public
Itemless compatibility gate with an outdated client.

Automated tests cover queue-mode confirmation, party compatibility, and lobby
labels. Eight-client gameplay has not been verified. Automated coverage does not
establish full multiplayer behavior for every party, rematch, or host-change
combination.
