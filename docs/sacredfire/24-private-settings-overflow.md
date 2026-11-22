# Private Cup: corrupt free after the race, then a lobby crash

## Observed
v0.8.3, two Windows players, private Cup with AI. The player who joined the other's room
crashes. When the racers unload on the way back to the lobby, `bailing corrupt free at guest
offset 0x1590e5d`. When the lobby next spawns the other player's kart, a second corrupt block
in the same pool, then a fault at guest 0x1490000 (a jump into the middle of code). Same pool,
callers and crash address on both machines.

## The dumps
The free at 0x1590e10 is TLSF: header at block - 0x10, size at block - 8, next header at
`block + ((size - 8) & ~3)`. With a size of 0..3 that "next header" is the block's own size
field, which then holds the header address + 1, and the `| 2` meant for the next size lands
on the object's vtable. Both dumps show exactly that, so the block's size (really 0x48) had
been set to a small value before the free.

The block before it holds a `COctaneLobbyPrivateMatchRaceData` (vtable 0x26a93e0, registered
at 0x19a4d38 with 0x28 bytes: +0x10 map, +0x20 mode, +0x24 laps). Its +0x28 is the next
block's size field. The 65 (0x41: a size with the free bit) read there before a corrupt free
on 19/11 is such a size field.

## Cause and fix
The team count is at `*race_settings + 0x28` (a real field: the game reads it at 0x9c1402).
Three places also used race manager +0xde8 + 0x28, the private-match data above:
`reset_team_count()` (leaving a followed room, the Race Type selector) wrote 1 there, and the
team lobby's "came back" and "new room" steps read it and wrote 1..4 there. Each write
overwrote the next block's size. Only the native race settings are used now. The test checks
8 guard bytes after a 0x28-byte stand-in.

`fa9d5c2` (guests leave the last Cup race without dropping the party) avoids one way of
reaching `reset_team_count()`; the corrupt-free recovery and the 0x1490000 skip in probe.c
handle the consequences.

Not tested in game.
