# Controllers

- `BB_GAMEPAD=<part of the name or SDL GUID>`: the controller that starts active on port 1; while
  it is not connected the first gamepad is used and the choice is retried every second.
- Inside the container SDL receives no hot-plug notifications (no udev). While the chosen
  controller is missing, the runtime restarts SDL's gamepad subsystem every 5 s to enumerate
  devices afresh (`Runtime: chosen gamepad "..." not seen; devices re-enumerated`); a
  reconnected Steam Controller is picked up within a few seconds (confirmed, run 29).
- The controller used last becomes the active one for port 1: all connected gamepads stay
  open, one that produces input while the active pad is idle takes over
  (`Runtime: gamepad switched to ...`).
- Split screen: the in-game menu (Insert or L3+R3) has a "Controllers (split screen)" section.
  Port 1 is the player; ports 2..4 take one connected gamepad each. The choice is saved to
  bbport.ini (`port.N=<GUID>`), the runtime re-reads the file within a second, opens that
  gamepad for the port and logs user N in (UserService login event, name "Player N") while it
  is connected; `scePadOpen(user N)` returns handle N and reads that pad only (no keyboard).
  Unassigning a port logs the user out. Confirmed 2026-10-09: a second gamepad assigned to port 2
  logged user 2 in ("user 2 logged in (controller port 2)") and joined a split-screen session.

## Rescan (2026-10-11)

The Steam Deck sometimes shows a controller connected after launch only to a fresh enumeration.
*Rescan controllers* in the in-game menu (section *Other*) restarts SDL's gamepad subsystem on the
next input sample (`bbgpu_pad_rescan_take`, overlay to runtime); the runtime also rescans every 5 s
while port 1 has no pad or a bound port 2..4 lacks its controller, up to 6 misses in a row
(`PAD_RESCAN_ATTEMPTS` in runtime_pad.c), after which the controller counts as disconnected. A
binding change in bbport.ini or the button starts the attempts again. The log says
`Runtime: controllers re-enumerated on request: N found` or `... devices re-enumerated: N found`.
