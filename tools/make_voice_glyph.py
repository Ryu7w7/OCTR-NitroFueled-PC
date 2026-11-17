#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws scripts/assets/voice_glyph.png: the voice chat's loudspeaker as a character of the game's
fonts (scripts/button_prompts.py puts it in their button image), white with a dark edge like
the nicknames over the karts, in the 83x89 cell it takes there. Needs Pillow."""
from pathlib import Path

from PIL import Image, ImageDraw

W, H = 83, 89
S = 8  # drawn this many times bigger, then reduced (smooth edges)
OUT = Path(__file__).resolve().parents[1] / 'scripts' / 'assets' / 'voice_glyph.png'


def shapes(draw, grow, colour):
    """The loudspeaker and two sound waves, each grown by grow (the dark edge under the white)."""
    s = S
    mid = H * s / 2
    # the box and the cone
    draw.polygon([(6 * s - grow, mid - 11 * s - grow), (19 * s, mid - 11 * s - grow), (36 * s + grow, mid - 26 * s - grow),
                  (36 * s + grow, mid + 26 * s + grow), (19 * s, mid + 11 * s + grow), (6 * s - grow, mid + 11 * s + grow)],
                 fill=colour)
    # the waves: arcs around the cone's mouth
    for radius, width in ((16, 7), (29, 7)):
        r = radius * s + grow  # an arc's width grows inwards from its box
        box = (36 * s - r, mid - r, 36 * s + r, mid + r)
        draw.arc(box, -48, 48, fill=colour, width=width * s + 2 * grow)


def main():
    big = Image.new('RGBA', (W * S, H * S), (0, 0, 0, 0))
    draw = ImageDraw.Draw(big)
    shapes(draw, 5 * S, (16, 16, 24, 255))
    shapes(draw, 0, (255, 255, 255, 255))
    big.resize((W, H), Image.LANCZOS).save(OUT)
    print(OUT)


if __name__ == '__main__':
    main()
