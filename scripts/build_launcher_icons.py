#!/usr/bin/env python3
"""Render the shared application icon with desktop-safe padding (requires Pillow)."""
import argparse
from pathlib import Path

from PIL import Image, ImageOps

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path, help="Original square application artwork")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]

with Image.open(args.source) as source:
    artwork = ImageOps.contain(source.convert("RGBA"), (208, 208), Image.Resampling.LANCZOS)
icon = Image.new("RGBA", (256, 256))
icon.alpha_composite(artwork, ((256 - artwork.width) // 2, (256 - artwork.height) // 2))
icon.save(root / "launcher/io.github.sacredfire.Launcher.png")
icon.save(root / "packaging/windows/icons/ctr.ico", sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])
