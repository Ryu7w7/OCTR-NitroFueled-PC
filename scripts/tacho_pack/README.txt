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
- patches/RaceLeaders_hud.xml, HudOriginal.xml, ItemsMejorados.xml, NtropyPosicion.xml,
  SinItemsPortales.xml, BarraReservas.xml: executable patches, on by default. Each can be
  turned off in the launcher's Patches page.
- scripts/tacho_pack/manifest.json and payloads/: the game files it swaps
  (scripts/tacho_pack.py applies them).

Settings (bbport.ini):
- tacho_pack = 0 leaves the whole pack out.
- tacho_pack_menu = 1 adds "Hud Power Charged" (Yes: new HUD, No: the game's own, from the next
  race) and "Barra de reservas" (Yes/No) to Options > Game.

It is made for the USA game (CUSA13795), update 1.21. With another executable, its patches are
skipped whole and its game files stay the game's own.
