#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws the Xbox button prompts of scripts/button_prompts.py: scripts/button_prompts/xbox.png.

Our own drawing (no art of any console maker or game): dark glossy buttons like the game's, with
the letters in Lilita One (art/ctr-menu.ttf, SIL Open Font License 1.1). Each button fills the
cell of the PlayStation button it replaces, laid out as in the 1.21 update's font (SHEET);
button_prompts.py copies them to wherever a font has those cells. Needs Pillow (only here: the
game's launch reads the PNG with button_prompts.read_png).

usage: python tools/make_button_prompts.py
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from button_prompts import ART, SHEET  # noqa: E402  (each button's cell, as in the 1.21 font)

FONT = ROOT / 'art' / 'ctr-menu.ttf'
OUT = ART / 'xbox.png'
WIDTH, HEIGHT = 512, 263
SCALE = 4  # drawn at 4x, then reduced: smooth edges
GREEN, RED, BLUE, YELLOW = (108, 198, 68), (232, 72, 64), (64, 150, 240), (246, 196, 26)
SHOULDER = (72, 170, 250)  # the blue of the game's L1/R1/L2/R2 labels


def font(size):
    return ImageFont.truetype(str(FONT), size * SCALE)


def body(size, shape, radius=0):
    """A dark glossy button: (image, mask) of size, shape 'circle', 'pill' or 'trigger'."""
    w, h = size[0] * SCALE, size[1] * SCALE
    mask = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(mask)
    if shape == 'circle':
        d.ellipse((0, 0, w - 1, h - 1), fill=255)
    elif shape == 'pill':
        d.rounded_rectangle((0, 0, w - 1, h - 1), radius=radius * SCALE, fill=255)
    else:  # trigger: rounded top, straight sides widening a little to the bottom
        top = int(w * 0.16)
        d.rounded_rectangle((top, 0, w - 1 - top, h * 0.6), radius=int(w * 0.32), fill=255)
        d.polygon([(top, h * 0.3), (w - 1 - top, h * 0.3), (w - 1, h - 1), (0, h - 1)], fill=255)
    # vertical gradient, lighter at the top, and a soft rim
    shade = Image.new('RGBA', (w, h))
    for y in range(h):
        t = y / max(1, h - 1)
        c = int(70 - 62 * t)
        ImageDraw.Draw(shade).line([(0, y), (w, y)], fill=(c, c, c + 4, 255))
    # a light rim like the game's buttons: the shape's outline (circles and pills), else the
    # mask's outer band (a round erosion: blurred, then only what stays fully inside)
    rim = Image.new('L', (w, h), 0)
    if shape == 'circle':
        ImageDraw.Draw(rim).ellipse((0, 0, w - 1, h - 1), outline=255, width=3 * SCALE)
    elif shape == 'pill':
        ImageDraw.Draw(rim).rounded_rectangle((0, 0, w - 1, h - 1), radius=radius * SCALE, outline=255,
                                              width=3 * SCALE)
    else:
        inner = mask.filter(ImageFilter.GaussianBlur(2 * SCALE)).point(lambda v: 255 if v >= 250 else 0)
        rim = Image.composite(Image.new('L', (w, h), 255), rim, Image.eval(inner, lambda v: 255 - v))
    rim = Image.composite(rim, Image.new('L', (w, h), 0), mask).filter(ImageFilter.GaussianBlur(SCALE))
    shade.paste((128, 134, 146, 255), (0, 0), rim)
    gloss = Image.new('L', (w, h), 0)
    ImageDraw.Draw(gloss).ellipse((w * 0.14, h * 0.04, w * 0.86, h * 0.40), fill=34)
    shade.paste((255, 255, 255, 255), (0, 0), gloss.filter(ImageFilter.GaussianBlur(2 * SCALE)))
    shade.putalpha(mask)
    return shade


def label(image, text, size, color, dy=0.0):
    """Text centered on image, with a glow of its color."""
    w, h = image.size
    layer = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    f = font(size)
    box = d.textbbox((0, 0), text, font=f)
    x = (w - (box[2] - box[0])) / 2 - box[0]
    y = (h - (box[3] - box[1])) / 2 - box[1] + dy * h
    d.text((x, y), text, font=f, fill=color + (255,))
    glow = layer.filter(ImageFilter.GaussianBlur(3 * SCALE))
    image.alpha_composite(glow)
    image.alpha_composite(glow)
    image.alpha_composite(layer)


def view_icon(image):
    """The View button: two overlapping windows."""
    w, h = image.size
    d = ImageDraw.Draw(image)
    s = w * 0.22
    for dx, dy in ((-0.07, -0.06), (0.07, 0.06)):
        cx, cy = w / 2 + dx * w, h / 2 + dy * h
        d.rounded_rectangle((cx - s, cy - s * 0.78, cx + s, cy + s * 0.78), radius=SCALE * 3,
                            outline=(225, 230, 240, 255), width=SCALE * 4, fill=(18, 18, 22, 255))


def button(name, size):
    cw, ch = size
    if name in ('Cross', 'Circle', 'Square', 'Triangle'):
        d = min(cw, ch) - 4
        img = body((d, d), 'circle')
        letter, color = {'Cross': ('A', GREEN), 'Circle': ('B', RED), 'Square': ('X', BLUE),
                         'Triangle': ('Y', YELLOW)}[name]
        label(img, letter, 60, color, dy=0.03)
    elif name in ('L1', 'R1'):
        img = body((cw - 4, ch - 6), 'pill', radius=(ch - 6) // 2)
        label(img, 'LB' if name == 'L1' else 'RB', 44, SHOULDER, dy=0.03)
    elif name in ('L2', 'R2'):
        img = body((cw - 8, ch - 4), 'trigger')
        label(img, 'LT' if name == 'L2' else 'RT', 38, SHOULDER, dy=0.14)
    else:  # Touchpad -> View
        d = min(cw, ch) - 4
        img = body((d, d), 'circle')
        view_icon(img)
    small = img.resize((img.width // SCALE, img.height // SCALE), Image.LANCZOS)
    cell = Image.new('RGBA', size, (0, 0, 0, 0))
    cell.alpha_composite(small, ((cw - small.width) // 2, (ch - small.height) // 2))
    return cell


def main():
    sheet = Image.new('RGBA', (WIDTH, HEIGHT), (0, 0, 0, 0))
    for name, (x0, y0, x1, y1) in SHEET.items():
        sheet.alpha_composite(button(name, (x1 - x0, y1 - y0)), (x0, y0))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(OUT, optimize=True)
    print(f'{OUT.relative_to(ROOT)}: {len(SHEET)} buttons')


if __name__ == '__main__':
    main()
