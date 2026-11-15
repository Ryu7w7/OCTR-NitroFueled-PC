# Online through Epic Online Services (EOS bridge)

Some players cannot reach the RyuNET server at all: their internet provider filters its name or
its address, or the ports on the way are closed. For them, the launcher runs **ryunet-bridge**
(`eosbridge/`). It carries the traffic through Epic Online Services (EOS) to a gateway that runs
next to the server. No Epic account is needed and no ports have to be opened.

```
player's PC                                  Epic                     server machine
game / launcher / run.py ── 127.0.0.1 ──► ryunet-bridge ══ EOS P2P (relays) ══► ryunet-bridge --gateway ── 127.0.0.1 ──► ryunet-server
```

Nothing else moves. The shop, Wumpa coins, crates, unlocks, accounts and matchmaking stay on the
RyuNET server. The bridge only gives players who cannot reach it another way in.

## How it works

1. **The check.** The launcher starts `eos\ryunet-bridge.exe --client` on Windows or
   `eos/ryunet-bridge --client` on Linux. It tries the server
   directly: `GET /ryunet/theme` (it must answer JSON), a lobby TCP connection, and a NAT probe on
   UDP 3074. If all three work, it prints `BRIDGE DIRECT` and exits. Players whose network works
   see no change.
2. **The bridge.** Otherwise it logs in to EOS anonymously (Connect, Device ID), opens a P2P
   connection to the gateway and offers local ports: one UDP port, plus one TCP port per server
   port (3074–3080, 8460, 8080). Then it prints:
   `BRIDGE READY ip=<server ip> vip=<virtual ip> map=<udp>,<server port>:<local port>,...`
3. **Who uses it.**
   - The launcher's requests: `EosBridgeRoute` in the shared `launcher/eos.inc`.
   - run.py and the cloud scripts: `run.bridge_port`.
   - The game: `src/runtime_net_bridge.c`. It reads `BB_ONLINE_BRIDGE` (the map) and
     `BB_ONLINE_SERVER_IP`, and from then on every connection and datagram to the server goes to
     the bridge's local port for it. The bridge's datagrams come back as if the server sent them.
4. **The player's address.** The gateway gives each player a virtual address in 198.18.0.0/15,
   the same one every time (`data/gateway-ips.txt`). The server sees the player by that address:
   - for TCP, through a `PROXY TCP4` line that the gateway sends first;
   - for UDP, through a `RYUGW1` header on the NAT port (`server/eos_gateway.go`).
   Bans, limits, NAT replies and peers' addresses all use that virtual address.
5. **Races.** Other players cannot reach a virtual address directly. So a bridged player's
   traffic with peers on the internet goes only through the server's tunnel (`nat_tunnel.go`),
   and the peers do the same toward it. LAN peers are still reached directly. Bridged players do
   not use the regional relays.

## Races through EOS P2P

Races are peer to peer. Two players whose direct path fails (strict NAT, carrier NAT, or a
bridged player) used to need the server's tunnel or the regional relays. With EOS set up, their
race traffic goes over EOS P2P instead. It connects directly when it can, and through Epic's relays
when it cannot. This is the use EOS P2P is made for. It works whether the server is reached
directly or through the bridge:

1. **The bridge stays.** After `BRIDGE DIRECT`, the bridge stays when EOS is set up, logs in, and
   prints `BRIDGE PEERS port=<udp> id=<id>`. The launcher hands that port to the game as
   `BB_ONLINE_EOS`. Bridged players get the same line.
2. **Capability.** The game marks its tunnelled copies with `TUN_FLAG_EOS` (bit 2 of the tunnel
   flags).
3. **Announce.** A peer that sees the flag announces its EOS id (`RYUEP1 A`), directly and
   through the tunnel. Older ports never get an announce.
4. **Allow.** The game tells its bridge about the peers it learned (`K`). The bridge accepts P2P
   connections only from those peers, on the pair's own socket (`RYP` plus a hash of both ids, as
   Epic's documentation recommends).
5. **Send.** The game's datagrams for a known peer also go to the bridge (`S`), which sends them
   over EOS. Datagrams coming back over EOS (`R`) reach the game as that peer's.
6. **Switch.** While the peer's datagrams keep arriving over EOS, its tunnelled copies stop. If EOS
   goes quiet for 2.5 s, the tunnel comes back by itself. If the direct path works both ways,
   nothing goes over EOS.

The game's side is `src/runtime_net_eos.c` (tested by `tests/test_net_eos.c`). The bridge's side is
`peer_*` in `eosbridge/eosbridge.c`.

## Lifetime

The bridge stays alive as long as its stdin is open. The launcher holds one end, and run.py
inherits it. So a game still running after the launcher closes keeps its bridge, and the bridge
exits when both are gone. Only one client runs per Windows user, because they all share the PC's
EOS identity. A second one exits with `BRIDGE FAILED busy`.

The launcher's Online tab has the setting **Connection through Epic Online Services**
(`launcher.ini eos_bridge`):

- **Automatic**: the check decides.
- **Always**: use the bridge even when the server answers directly, for testing.
- **Never**: never use the bridge.

## Setting it up

### 1. Create the Epic product

The Developer Portal is free (dev.epicgames.com/portal):

1. Create a product.
2. Under **Product Settings > Clients**, add a client with the **Peer2Peer** client policy, or a
   custom policy that allows Connect and P2P. Epic Account Services are not needed.
3. **SDK Download & Credentials** shows the product, sandbox and deployment ids, and the client's
   id and secret. Use the **Dev** sandbox's deployment for testing.
4. Copy `eosbridge/eosbridge.ini.example` to `eosbridge/eosbridge.ini` and fill in the ids. This
   file is not in git.

### 2. Build the game and EOS together

This is maintainer setup, not a requirement for players. Windows and Linux use the same
`eosbridge/eosbridge.c` and product configuration; each build links or loads its own Epic SDK
library. The Windows build uses MSYS2 CLANG64 and the Linux build uses the native compiler.

Download Epic's SDK and complete `eosbridge/eosbridge.ini` from step 1. The SDK directory must
contain `SDK/Include/eos_sdk.h` and the runtime for the target platform:

| Platform | Required SDK files | Build output |
| --- | --- | --- |
| Windows x64 | `SDK/Bin/EOSSDK-Win64-Shipping.dll`, `SDK/Lib/EOSSDK-Win64-Shipping.lib` | `out/eosbridge/ryunet-bridge.exe` and the DLL |
| Linux x64 | `SDK/Bin/libEOSSDK-Linux-Shipping.so` | `out/eosbridge/ryunet-bridge` and the shared library |

Then use the normal build command on either platform:

```bash
EOS_SDK=/path/to/EOS-SDK bash build.sh
```

On Windows, run this in MSYS2 CLANG64 and use an MSYS path such as `/c/SDK/EOS-SDK`.
`~/Downloads/EOS-SDK-*` and the download cache at `out/deps/eos-sdk` are also detected.
Linux's standalone `bash launcher/build.sh` includes the same EOS build hook.
There is no separate EOS compile command to remember. Configuration is embedded by the existing
recipe, and packaging copies the native executable and runtime library. SDK headers and the
plaintext product configuration are not included in the player packages.

Without any EOS configuration, the build explicitly skips EOS and keeps direct RyuNET available.
Once EOS inputs are supplied, incomplete configuration, download errors, and compilation errors
fail the build instead of producing a release that claims to contain working EOS.

#### One CI run produces both platforms

For Ryu or another repository maintainer, configure these repository Actions secrets once:

- `EOS_SDK_URL`: an HTTPS ZIP URL that runners can download. The archive must contain the SDK
  headers, Windows import library/DLL, and Linux shared library listed above.
- `EOS_BRIDGE_INI`: the complete product configuration from `eosbridge.ini`, including the
  gateway's `gateway_id` for the relay connection described below. Both platforms use it.

Run the existing **Build and release** workflow, or push a release tag. Its Windows and Linux
jobs both pass those secrets into their normal `bash build.sh` command. The shared auto-build
hook downloads/prepares the SDK, validates the required product fields, embeds the configuration,
and builds the matching bridge. The resulting Windows ZIP and Linux AppImage include EOS.
A local build produces its current platform; the workflow's two runners produce both in one run.

Downloads are cached locally at `out/deps/eos-sdk`; an existing `EOS_SDK` directory avoids another
download. To change the downloaded SDK version, remove that cache or point `EOS_SDK` at the new SDK.
`eosbridge/eosbridge.ini` remains ignored by Git. Do not put private SDK URLs or product
configuration into workflow YAML or commit them to the repository.

The standalone bridge recipe remains available for gateway development:

```bash
bash eosbridge/build.sh          # current platform, without rebuilding the game
bash eosbridge/build.sh linux    # Linux gateway from Windows through WSL
```

The explicit `linux` gateway target uses compatibility flags and reports its required glibc
version. Normal Linux game builds use the current native/Nix toolchain and package its runtime.

### 3. Run the gateway next to the server

1. Copy `ryunet-bridge`, `libEOSSDK-Linux-Shipping.so` and `eosbridge.ini` (without `gateway_id`)
   to a folder on the server.
2. Install `eosbridge/ryunet-eos-gateway.service` and set `--server-ip` to the server's public
   address.
3. On the first start it prints `GATEWAY ID <id>`. That id goes into the clients' `eosbridge.ini`
   as `gateway_id`.
4. Keep its `data/` folder: it holds the gateway's identity and the players' addresses.
5. In the server's `.env`, set `RYUNET_EOS_GATEWAY=1` and restart the server. It then believes
   PROXY lines and `RYUGW1` packets only from its own machine. `RYUNET_EOS_GATEWAY_FROM` names
   other addresses if the gateway runs elsewhere.

### 4. Ship it to players

When `eosbridge/eosbridge.ini` exists, `eosbridge/build.sh` builds its settings into
`ryunet-bridge.exe` (`embed_settings.py`), so no settings file ships with the game. Before the
release build, put the production gateway's `gateway_id` in it.
`packaging/windows/make_portable.py` adds the `eos\` folder (the exe and the DLL).

A client's EOS credentials are meant to ship inside the game. Anyone can still dig them out of
the exe; building them in only keeps them from being a file to copy. Their reach is what the
client policy allows. Keep it minimal (Connect and P2P), restrict the gateway's own client to the
server's address ("Trusted server" in the portal), and rotate the client if the portal shows use
that is not yours. An `eosbridge.ini` next to the exe overrides what is built in.

**Unofficial builds** (forks, other servers) must use their own Epic product: it is free, and
Epic's agreement does not let credentials be shared outside its licensed developers. Without
`eosbridge/eosbridge.ini` the bridge has no settings, the launcher reports it unavailable, and the
game connects directly, as it does without EOS.

## Testing without Epic

`--debug-listen` (on the gateway) and `--debug-gateway` (on the client) replace EOS with a plain
TCP connection that carries the same messages:

```bash
ryunet-bridge --gateway --server-ip 203.0.113.5 --no-eos --debug-listen 127.0.0.1:39800
ryunet-bridge --client --server 127.0.0.1 --mode on --debug-gateway 127.0.0.1:39800
curl http://127.0.0.1:39709/ryunet/theme     # the server's answer, through both ends
```

`tests/test_net_bridge.c` (`build.sh --test`) checks the game's side against a fake bridge. The
server's side is covered by `server/eos_gateway_test.go`.

## Limits

- **Logging in.** For a bridged player:
  - **Nickname and password:** works. The launcher sends it through the bridge, and the server
    counts the gateway's players as secure (`requestSecure`), because EOS P2P is encrypted up to
    the gateway on its machine.
  - **Link with Discord:** works. Discord's redirect goes to 127.0.0.1, and the link goes to the
    server through the bridge.
  - **Log in on the site:** the site itself may not open on that network. The launcher says to
    open the link on a phone (on mobile data) or to use one of the two ways above.
- **Latency.** Once both players have EOS, a race goes player → Epic (direct or relay) → peer,
  without the gateway. Only with a peer that has no EOS does a bridged player's race go through
  the gateway and the server's tunnel, which is slower.
- **Epic's terms.** The P2P service is for connections between game clients, with relays for
  restrictive networks, which is what the races use. The gateway is a client too, but it carries
  other players' server traffic. Keep it to that: login, lobby and HTTP, which is little, and only
  for players who reach nothing else. If in doubt, ask Epic through the Developer Portal.
- **Linux.** The Linux launcher does not start the bridge yet. run.py and the game already follow
  `BB_ONLINE_BRIDGE` when it is set.


### Shared Linux launcher and CI

The native Linux launcher uses the same bridge status, settings, routing protocol and game
environment as Windows. Place `ryunet-bridge` and `libEOSSDK-Linux-Shipping.so` in `eos/` next
to the launcher scripts, or build them with `EOS_SDK=/path/to/sdk bash eosbridge/build.sh` and
include `out/eosbridge/` through the AppImage package. The bridge's product configuration is
embedded when `eosbridge/eosbridge.ini` exists; it is not copied as plaintext into the package.

Release CI accepts `EOS_SDK_URL` and `EOS_BRIDGE_INI` repository secrets. Without those secrets,
the package retains direct RyuNET and reports that the EOS bridge is missing. Launcher protocol
and process tests use a local fixture; actual Epic login requires the licensed SDK and valid
product settings. Neither the SDK nor product secrets belong in source control.
