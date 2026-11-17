"""Writes src/runtime_texts.inc (the runtime's copy of the in-game text table) from scripts/octr_texts.py.
Usage: python tools/gen_octr_texts.py
SPDX-License-Identifier: GPL-2.0-or-later
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import octr_texts  # noqa: E402

target = ROOT / 'src' / 'runtime_texts.inc'
target.write_bytes(octr_texts.c_table().encode('utf-8'))
print(f'{target.relative_to(ROOT)}: {len(octr_texts.TABLE)} texts')
