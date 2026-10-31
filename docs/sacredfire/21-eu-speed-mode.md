# EU 1.21 private Speed Mode

On CUSA14876 1.21, the native Game Mode selector was left unchanged because both
startup paths restricted menu and Speed overlay preparation to CUSA13795. The EU
executable also uses different code addresses and a different menu asset hash.

`run.sh` is the Linux/AppImage entry path; updating only `run.py` leaves EU launches
without `BB_ONLINE_MODE_HOOK` and the private overlay list. Both entry paths now
admit USA/EU 1.21, while the preparation and runtime checks reject unknown layouts.

The EU dump uses choice adapter `0x1277df0`, selector event `0x1277c60`, base event
`0x125d560`, private-state reader `0xf50340`, and season setter `0xfb7700`.
These five functions are 16 bytes earlier than their USA counterparts. The event slot
`0x2773b40`, initialize type `0x2a21ee8`, network pointer `0x2acb3d8`, and choice
adapter's RIP-relative target `0x2af1fa8` are shared. The installer validates the
region's prologue and event pointer before writing hooks.

The EU selector asset has SHA-256
`cdb4b366079458a0f1ed32fb8a8ac69d2d841f18bacb9a72430a02e93fcf52a2`;
its option and availability lists retain offsets `0x1880` and `0x18a8`.
Both original asset hashes are accepted; unknown assets remain rejected. Launch preparation
only enables this feature for these two title IDs at version `01.21`, in both
`run.sh` (Linux/AppImage) and `run.py` (Windows).
Public Speed matchmaking remains disabled. RyuNET's existing `cusa13795-session-`
namespace is shared by both editions and is not changed by this support.

Validation: the native adapter tests exercise both address layouts, including private/public
navigation and seasonal rules; the shell startup test verifies that Linux exports
the hook and both overlay lists for each region. `BB_TEST_GAME=/path/to/game python3 -m unittest discover
-s tests -p test_online_mode.py` also checks the actual executable and menu asset.
Full menu and Speed overlay preparation was checked against both local 1.21 dumps:
40 tracks and five matching normal/private archive overlays per region. Live multiplayer
and a Windows build still need verification.
