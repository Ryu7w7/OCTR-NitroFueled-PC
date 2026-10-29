"""Native Normal Mode / Speed Mode / Battle Mode options for the shared online mode selector.

The runtime keeps Speed disabled in public matchmaking until server queues support it.
"""
import argparse
import hashlib
import re
import struct
from pathlib import Path

import archive_patch
import gcn_shaders as iga
from igz_file import Igz
from mod_file_paths import resolve
from patches import load_segments

ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_modeselect.igz'
LABEL_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_modeselect_en.lng'
GRAPH_SHA256 = '56d613eaa1234151a3d720af694b460480c4cbc766508cb60c324c91ca8eae47'
# Native selector's selected-index event adapter, including its RIP-relative LEA.
HOOK_PATTERN = re.compile(bytes.fromhex(
    '55 48 89 e5 41 57 41 56 53 50 48 8d 05 97 a1 87 01'), re.DOTALL)


def hook_site(elf):
    hits = [v + m.start() for v, off, size in load_segments(elf)
            for m in HOOK_PATTERN.finditer(elf[off:off + size])]
    if hits != [0x1277e00]:
        raise ValueError('native Online selector adapter is not uniquely recognized')
    return hits[0]


def rebuild(blob):
    if hashlib.sha256(blob).hexdigest() != GRAPH_SHA256:
        raise ValueError('native Online mode selector is not the supported USA 1.21 asset')
    g = Igz(blob)
    # The native selector consumes the same option/availability lists as other selectors.
    for lst, raw, tag in (
        (0x1880, b''.join(struct.pack('<Q', g.string_index(s))
                         for s in ('NORMAL MODE', 'SPEED MODE', 'BATTLE MODE')), 'RSTT'),
        (0x18a8, bytes((1, 1, 1)), None),
    ):
        array = g.append(raw)
        g.set_u32(lst + 0x0c, 3)
        g.set_u32(lst + 0x10, 3)
        # Embedded archive buffers must not be freed as heap allocations on menu unload.
        g.set_u64(lst + 0x18, (g.u64(lst + 0x18) & ~0xffffffff) | (1 << 56) | len(raw))
        g.set_u64(lst + 0x20, array)
        g.add_site('ROFS', lst + 0x20)
        if tag:
            for i in range(3):
                g.add_site(tag, array + i * 8)
    return g.build()


def prepare(game, out, elf, bases):
    site = hook_site(Path(elf).read_bytes())
    pak = resolve(Path(game) / 'archives/update2.pak')
    entries = iga.pak_entries(pak)
    index = next((i for i, e in enumerate(entries) if e[0].lower() == ENTRY), None)
    if index is None:
        raise ValueError('native Online menu graph is missing')
    with pak.open('rb') as f:
        payload = rebuild(iga.read_entry(f, *entries[index][1:]))
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    guest = '/app0/archives/update2.pak'
    patches = {archive_patch.read(p)[1]: Path(p) for p in bases}
    target = out / 'online-mode.bbpatch'
    archive_patch.write_entries_patch(target, pak, guest, {index: payload}, patches.get(guest))
    patches[guest] = target
    label_pak = resolve(Path(game) / 'archives/update.pak')
    label_entries = iga.pak_entries(label_pak)
    label_index = next(i for i, e in enumerate(label_entries) if e[0].lower() == LABEL_ENTRY)
    with label_pak.open('rb') as f:
        label = Igz(iga.read_entry(f, *label_entries[label_index][1:]))
    if label.strings.count('Racing Type') != 1:
        raise ValueError('native Online mode label is not recognized')
    label.strings[label.strings.index('Racing Type')] = 'GAME MODE'
    guest = '/app0/archives/update.pak'
    target = out / 'online-mode-label.bbpatch'
    archive_patch.write_entries_patch(target, label_pak, guest, {label_index: label.build()}, patches.get(guest))
    patches[guest] = target
    return list(patches.values()), f'{site:x}'


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--elf', required=True, type=Path)
    p.add_argument('--base', action='append', default=[], type=Path)
    a = p.parse_args()
    patches, hook = prepare(a.game, a.out, a.elf, a.base)
    print('archive_patches=' + '|'.join(map(str, patches)))
    print('online_mode_hook=' + hook)
