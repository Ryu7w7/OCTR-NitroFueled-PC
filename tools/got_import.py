"""Diagnostic: name the import bound to a GOT slot (guest offsets) in out/boot.bin."""
import argparse, re, struct
from pathlib import Path
here = Path(__file__).resolve().parent.parent  # repository root
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--boot-dir', type=Path, help='boot directory printed by the launcher (legacy default: out/)')
p.add_argument('slots', nargs='*', help='hexadecimal GOT offsets')
a = p.parse_args()
if a.boot_dir is None:
    if (here / 'out/boot-cache').exists():
        p.error('Pass --boot-dir from the launcher log; legacy out/boot.bin may belong to a different game')
    a.boot_dir = here / 'out'
data = (a.boot_dir / 'boot.bin').read_bytes()
size, entry, ns, nr, ni, caps = struct.unpack_from('<QQQQQQ', data, 8)
pos = 56 + ns * 24
names = [data[pos + i * 128:pos + (i + 1) * 128].split(b'\0')[0].decode() for i in range(ni)]
pos += ni * 128
symbols = dict(re.findall(r'\{"([^"]+)","([^"]+)"\}', (here / 'src/import_names.inc').read_text()))
slots = {}
for i in range(nr):
    target, kind, value, addend = struct.unpack_from('<QQqq', data, pos + i * 32)
    if kind:
        slots[target] = names[value]
for arg in a.slots:
    nid = slots.get(int(arg, 16))
    print(arg, nid, symbols.get(nid) if nid else '')
