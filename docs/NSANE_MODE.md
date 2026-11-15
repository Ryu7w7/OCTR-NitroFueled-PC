# N.Sane Mode (private rooms)

The private room's Game Mode selector has a fourth option, **N.SANE MODE** (NORMAL / ITEMLESS / BATTLE /
N.SANE MODE). In an N.Sane race a player can hold a second item:

- With an item in hand (or the roulette spinning), a crate the game would ignore stores a second item, with the
  game's own position odds. The rows of crates have extra crates in N.Sane races (scripts/nsane_crates.py).
- On the Grand Prix tracks (no extra crates) one crate gives both items.
- A small slot beside the item HUD shows the stored item, after its own roulette. It waits for the first
  roulette and never repeats the first item (unless the position's odds allow only that item).
- Using the first item gives the second one directly (no roulette).

Outside N.Sane races nothing changes: the hooks pass straight to the game and the slot stays hidden.

## Pieces

| Piece | Where |
| --- | --- |
| Selector option (index 3; BATTLE keeps index 2) | `scripts/online_mode.py` `rebuild`, `src/runtime_online_mode.c` `mode_selector` / `select_mode` / `selector_event` |
| Room mode for joining players | `src/runtime_room_mode.c`: `POST/GET /ryunet/room-mode` with `mode=nsane` |
| Mode flag and race gate | `src/runtime_file.c` `runtime_nsane_mode_set` / `runtime_nsane_race_active` |
| Second item and HUD slot | `src/runtime_nsane.c` (hooks 0xe36230 crate collected, 0xeb10d0 item used) |
| Hidden HUD slot in every item HUD view | `scripts/nsane_mode.py`, called by `scripts/speed_mode.py` |

The HUD slot is in the normal and the Itemless views alike, so their archive layouts stay identical (the
runtime requires it); it is hidden (flag 0x400, alpha 0) until `runtime_nsane.c` shows it.

## RyuNET requirement

Joining players follow the host's mode through `/ryunet/room-mode`. The server must accept and return a
third value:

- `POST /ryunet/room-mode?name=<host>&mode=nsane&format=text` -> `status ok`
- `GET /ryunet/room-mode?host=<host>&format=text` -> `mode nsane`

Until the server does, choosing N.SANE MODE in a private room fails the pre-race verification (the race does
not start and the native notice asks to retry); Normal, Itemless and Battle are unaffected.

## Support

USA 1.21 only (the hooks check their exact bytes). On another version `runtime_nsane_start` reports the mode as
unavailable and the selector keeps the option disabled.

## Verification

- `tests/test_nsane_mode.py` (BB_TEST_GAME): the slot's 13 icons + background are added, hidden, marked and
  bound to valid handles; a second pass is rejected.
- `tests/test_online_mode.py`: the selector carries four options.
- In game (as a standalone mod with the same code): storing, showing and handing over the second item works
  in offline races. Online N.Sane with RyuNET has not been tested.
