Pack Tacho 1.2.8, by Tacho
Built into the port with his permission. Before, it was installed as an OCTR mod package.

What it does:
- Race leaders list with 8 places, each racer's name, wumpa count and held item; the minimap
  sits a little higher so it does not cover the speedometer ("Hud Power Charged").
- Reserves bar: your turbo reserves as a bar and in seconds, under the turbo bar, in the three
  HUD styles. Technique adapted from Onur's "Reserves Meter" (MIT, LICENSE-Onur.txt).
- Power Charged logo and backgrounds in the main menu.
- Improved items: invisibility without the boost flame, Super Engine, TNT as a hot potato,
  the mask is immune to the train, your own ghost.
- N. Tropy's clock by position: shorter the further back its user is (x1.0 down to x0.5);
  racers behind the user are not affected.
- No items near the portals (Spyro Circuit and the space highway track).

Where it lives in the port:
- scripts/tacho_pack/patches/RaceLeaders_hud.xml, HudOriginal.xml, ItemsMejorados.xml,
  NtropyPosicion.xml, SinItemsPortales.xml, BarraReservas.xml: executable patches, always
  applied. They are part of the port, so they are not listed with the mods.
- scripts/tacho_pack/manifest.json and payloads/: the game files it swaps
  (scripts/tacho_pack.py applies them).

It has no launcher settings: every player races with the same pack. scripts/tacho_pack.py checks
these files against the SHA256 values written in it, and a changed or missing file stops the
launch until the port is installed again. Its Options > Game entries (as in Pack Tacho 1.3.0) are
in: "Hud Power Charged" (Yes: the new HUD, No: the game's own) and "Barra de reservas", which
starts off (src/runtime_reserves_bar.c) and is turned on there. They change only how a player's
own HUD looks.

It is made for the USA game (CUSA13795), update 1.21. With another executable, its patches are
skipped whole and its game files stay the game's own.
