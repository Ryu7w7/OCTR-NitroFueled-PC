#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The game's Options menu "Graphics" tab: relabelled menu text in an extra archive, update3.pak.

The Options menu (ui/Octane_Options_Menu.igz) has a fifth tab, Language Select, that the game
hides in this region. The port shows it again and opens its own settings menu when it is chosen
(gpu/shim/bbport_graphics_tab.cpp); this script renames it "Graphics" in the menu's available USA/EU text
tables (ui/Octane_Options_Menu_*.lng).

The game loads archives/update.pak, update2.pak, ... until one is missing, each overriding the
files of the ones before; the 1.21 update's own menu files are in update.pak/update2.pak. The
newest copy of each text table is read from the player's own archives, relabelled, and written
to a small update3.pak (uncompressed, the archives' own format) that the game loads last.

Diagnostics: the Options menu files of every archive searched are copied to
OUT/graphics-tab/dump/<archive>/ (a few hundred kilobytes).

Prints "archives/update3.pak=<file>" for scripts/mods.py --replace, or nothing when the
graphics_tab setting is off or the files are not found.

usage: graphics_tab.py GAME --out OUT [--settings bbport.ini]
"""
import argparse
from contextlib import ExitStack
import lzma
import struct
import sys
import zlib
from pathlib import Path

# Searched newest first: the first archive that has a file wins.
ARCHIVES = ('update2.pak', 'update.pak', 'juicedomain_permanent.pak')
OUTPUT = 'archives/update3.pak'
# The tab's label is the string before this placeable name in each text table.
ANCHOR = b'Comp__Menu_ButtonWithIcon005_behaviors_0'
LABELS = {'en': 'Graphics', 'fr': 'Graphismes', 'mx': 'Gráficos', 'pt': 'Gráficos',
          'es': 'Gráficos', 'de': 'Grafik', 'it': 'Grafica', 'ar': 'Graphics'}
DUMP = ('octane_options_menu', 'octane_options_languageselect', 'octane_mainmenu', 'octane_main_menu',
        'octane_startscreen_menu')
SECTOR = 0x800
VERSION = 19  # of this script's output (the cached file is rebuilt when it changes)


def fnv1a(text):
    h = 0x811C9DC5
    for c in text.lower().encode('utf-8'):
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


class Archive:
    """An igArchive (IGA version 13) read in place: the header and tables, then file data."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        head = self.f.read(0x38)
        (magic, version, _toc, count, sector, self.divider, self.slop, n_large, n_medium, n_small,
         name_off, name_size, self.flags) = struct.unpack('<IIIIIIIIIIQII', head)
        if magic != 0x1A414749 or version != 13 or sector != SECTOR:
            raise ValueError(f'{path.name}: unexpected archive header')
        tables = self.f.read(4 * count + 16 * count + 4 * n_large + 2 * n_medium + n_small)
        pos = 4 * count
        self.infos = [struct.unpack_from('<IiIi', tables, pos + 16 * i) for i in range(count)]
        pos += 16 * count
        self.large = struct.unpack_from(f'<{n_large}I', tables, pos); pos += 4 * n_large
        self.medium = struct.unpack_from(f'<{n_medium}H', tables, pos); pos += 2 * n_medium
        self.small = tables[pos:pos + n_small]
        self.f.seek(name_off)
        names = self.f.read(name_size)
        self.names = []
        for i in range(count):
            o = struct.unpack_from('<i', names, 4 * i)[0]
            end = names.index(b'\0', o)
            end2 = names.index(b'\0', end + 1)
            self.names.append((names[o:end].decode('utf-8', 'replace'),
                               names[end + 1:end2].decode('utf-8', 'replace')))

    def logical(self, i):
        full, logical = self.names[i]
        return (logical or full).replace('\\', '/')

    def read(self, i):
        offset, ordinal, size, block_index = self.infos[i]
        offset |= (ordinal & 0xFF) << 32  # archives past 4 GiB: the offset's high byte
        if block_index == -1:
            self.f.seek(offset)
            return self.f.read(size)
        comp = (block_index & 0xFFFFFFFF) >> 28
        first = block_index & 0x0FFFFFFF
        out = bytearray(size)
        for b in range((size + 0x7FFF) >> 15):
            bi = first + b
            if 0x7F * SECTOR < size:
                if 0x7FFF * SECTOR < size:
                    e, n, mask, bit = self.large[bi], self.large[bi + 1], 0x7FFFFFFF, 31
                else:
                    e, n, mask, bit = self.medium[bi], self.medium[bi + 1], 0x7FFF, 15
            else:
                e, n, mask, bit = self.small[bi], self.small[bi + 1], 0x7F, 7
            boff = (e & mask) * SECTOR
            bsize = (n & mask) * SECTOR - boff
            dsize = min(0x8000, size - b * 0x8000)
            self.f.seek(offset + boff)
            src = self.f.read(bsize)
            if not (e >> bit) & 1:
                chunk = src[:dsize]
            elif comp == 1:
                csize = struct.unpack_from('<H', src, 0)[0]
                chunk = zlib.decompress(src[2:2 + csize], -15)
            elif comp == 2:
                csize = struct.unpack_from('<I', src, 0)[0]
                alone = src[4:9] + struct.pack('<Q', dsize) + src[9:9 + csize]
                chunk = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(alone, dsize)
            else:
                raise ValueError(f'compression {comp}')
            out[b * 0x8000:b * 0x8000 + dsize] = chunk[:dsize]
        return bytes(out)


def relabel(text, label):
    k = text.find(ANCHOR)
    if k < 0 or text.count(ANCHOR) != 1:
        raise ValueError('tab label not found')
    j = k - 1
    while text[j] == 0:
        j -= 1
    start = text.rfind(b'\0', 0, j) + 1
    # The new label takes the old one's bytes, padded with spaces: the string table is read one
    # NUL-terminated string after another, so extra NULs would be empty strings that shift every
    # later one (OCTR v0.7: the first tab, Gameplay, last in the table, showed "Crash Cove").
    # Its terminator and alignment padding stay as they were, as in quit_labels.
    return text[:start] + same_length(text[start:j + 1], label) + text[j + 1:]


def write_archive(dest, files, template):
    """files: [(full name, logical name, ordinal, data)], uncompressed (block index -1)."""
    files = sorted(files, key=lambda f: fnv1a(f[1]))
    count = len(files)
    names = bytearray(4 * count)
    for i, (full, logical, _ordinal, _data) in enumerate(files):
        struct.pack_into('<i', names, 4 * i, len(names))
        names += full.encode('utf-8') + b'\0' + logical.encode('utf-8') + b'\0' + b'\0' * 4
    toc_size = 4 * count + 16 * count
    data_start = (0x38 + toc_size + SECTOR - 1) // SECTOR * SECTOR
    infos = bytearray()
    body = bytearray()
    for full, logical, ordinal, data in files:
        infos += struct.pack('<IiIi', data_start + len(body), ordinal, len(data), -1)
        body += data + b'\0' * (-len(data) % SECTOR)
    name_off = data_start + len(body)
    # The lookup: an id's expected position is id // divider, searched within +-slop
    # (essentialui.pak, 4 files: divider 0x3fffffff).
    divider = (1 << 32) // count - 1
    slop = max(abs(i - fnv1a(f[1]) // divider) for i, f in enumerate(files)) + 1
    head = struct.pack('<IIIIIIIIIIQII', 0x1A414749, 13, toc_size, count, SECTOR, divider,
                       slop, 0, 0, 0, name_off, len(names), template.flags)
    toc = b''.join(struct.pack('<I', fnv1a(f[1])) for f in files) + infos
    out = bytearray(head + toc)
    out += b'\0' * (data_start - len(out))
    out += body + names
    tmp = dest.with_suffix('.tmp')
    tmp.write_bytes(out)
    tmp.replace(dest)


def rename(old, new):
    """A same-length rename of the one string `old` in a visual script."""
    assert len(old) == len(new)

    def patch(data):
        if data.count(old + b'\0') != 1:
            raise ValueError(f'{old.decode()} not found once')
        return data.replace(old + b'\0', new + b'\0')
    return patch


def show_tab(data):
    """ui/Octane_Options_Menu.igz (1.21 update): the fifth tab's placeable (the fifth child of the
    Buttons placeable) has alpha 0, and stays 0 in the NoExtraButton and NoExtraAndTutorialsButtons
    layouts; only the Switch layout (NXLayout) fades it in. Its alpha and those two keyframes
    become 1. Offsets are in the file's data section, each checked first."""
    data = bytearray(data)
    if data[:4] != b'\x01ZGI':
        raise ValueError('Octane_Options_Menu.igz: not an igz file')
    base = struct.unpack_from('<I', data, 0x28)[0]
    placeable, keyframes = 0x1370, (0x2a30, 0x2b98)
    one = struct.pack('<f', 1.0)
    if (data[base + placeable + 0xe0:base + placeable + 0xe4] != b'\0' * 4 or
            data[base + 0x1250 + 0xe0:base + 0x1250 + 0xe4] != one):
        raise ValueError('Octane_Options_Menu.igz: unexpected layout')
    data[base + placeable + 0xe0:base + placeable + 0xe4] = one
    for k in keyframes:
        if struct.unpack_from('<q', data, base + k + 0x10)[0] != -1 or data[base + k + 0x18:base + k + 0x1c] != b'\0' * 4:
            raise ValueError('Octane_Options_Menu.igz: unexpected keyframe')
        data[base + k + 0x18:base + k + 0x1c] = one
    return bytes(data)


# Files changed in update3.pak: logical name -> change.
CHANGES = {f'ui/octane_options_menu_{lang}.lng': (lambda label: lambda data: relabel(data, label))(label)
           for lang, label in LABELS.items()}
CHANGES['ui/octane_options_menu.igz'] = show_tab
# The Options menu's root script hides the fifth tab in this region by name: the name is changed
# to one that matches nothing, so the hide does nothing.
CHANGES['vsc/ui/octane_options_menu/octane_options_menu_root.igz'] = rename(
    b'Comp__Menu_ButtonWithIcon005', b'Comp__Menu_ButtonWithIcon00X')
# The tab's own script opens the language screen by name: it opens a project that does not exist,
# which the port's open2_VSC hook takes as the Graphics tab (bbport_graphics_tab.cpp).
CHANGES['vsc/ui/octane_options_menu/octane_options_menu_languageselectbutton.igz'] = rename(
    b'Octane_Options_LanguageSelect', b'Octane_Options_BbportSettings')


# ---- The main menu's Quit Game button --------------------------------------------------------
# The main menu (ui/Octane_MainMenu.igz, 1.21 update) has a seventh button, Wireless
# (Comp__Menu_ButtonWithIcon), shown only in the Switch layout. It becomes Quit Game: relabelled,
# shown with the Switch layout's positions, reachable from its neighbours, and its "open level"
# names a map that does not exist, which the port's loadMap2_VSC hook takes as quitting.
QUIT_KEY = b'Comp__Menu_ButtonWithIcon_behaviors_0\0'
QUIT_LABELS = {'en': ('Quit Game', 'Close the game and return to the desktop.'),
               'fr': ('Quitter', 'Fermer le jeu et revenir au bureau.'),
               'mx': ('Salir', 'Cerrar el juego y volver al escritorio.'),
               'pt': ('Sair', 'Fechar o jogo e voltar para a area de trabalho.'),
               'es': ('Salir', 'Cerrar el juego y volver al escritorio.'),
               'de': ('Beenden', 'Spiel beenden und zum Desktop zurückkehren.'),
               'it': ('Esci', 'Chiudi il gioco e torna al desktop.'),
               'ar': ('Quit Game', 'Close the game and return to the desktop.')}
QUIT_LEVEL = (b'Octane/Octane_OnlineLobby/Octane_OnlineLobby\0',
              b'Octane/Octane_BbportQuit0/Octane_BbportQuit0\0')


def same_length(old, new):
    """`new` in `old`'s place: padded with spaces (the string table keeps its layout)."""
    new = new.encode('utf-8')
    if len(new) > len(old):
        raise ValueError(f'{new!r} longer than {old!r}')
    return new + b' ' * (len(old) - len(new))


def quit_labels(label, hint):
    def patch(data):
        k = data.find(QUIT_KEY)
        if k < 0 or data.count(QUIT_KEY) != 1:
            raise ValueError('Wireless label not found')
        # The hint before the key, the label after it.
        h_end = k
        while data[h_end - 1] == 0:
            h_end -= 1
        h_start = data.rfind(b'\0', 0, h_end) + 1
        l_start = k + len(QUIT_KEY)
        l_end = data.index(b'\0', l_start)
        room = l_end  # the label, its terminator and the padding to the next string
        while data[room] == 0:
            room += 1
        new = label.encode('utf-8')
        # Spaces before the terminator keep the strings' 2-byte alignment exactly as it was.
        size = room - l_start
        if len(new) + 1 > size:
            raise ValueError(f'{label!r} does not fit')
        if len(new) <= l_end - l_start:
            text = same_length(data[l_start:l_end], label) + data[l_end:room]
        else:
            text = new + b'\0' * (size - len(new))
            if size - len(new) != 1 and (size - len(new)) % 2 == 0:
                raise ValueError(f'{label!r}: no exact fit')
        return (data[:h_start] + same_length(data[h_start:h_end], hint) + data[h_end:l_start] +
                text + data[room:])
    return patch


def show_quit(data):
    """ui/Octane_MainMenu.igz: Wireless (shown only on the Switch) becomes visible, sits below
    Wumpa Challenges and is the last item of the menu; its level is renamed. Offsets in the
    file's data section, each checked first."""
    data = bytearray(data)
    if data[:4] != b'\x01ZGI':
        raise ValueError('Octane_MainMenu.igz: not an igz file')
    base = struct.unpack_from('<I', data, 0x28)[0]
    wireless, adventure, wumpa = 0x41d0, 0x4530, 0x3cf0  # placeables
    # Navigation items (up +0x110, down +0x118): Wumpa Challenges -> Quit Game -> Adventure.
    links = {0x7760 + 0x118: (adventure, wireless), 0x5988 + 0x110: (0x42f0, wumpa),
             0x5988 + 0x118: (0x40b0, adventure), 0x47b0 + 0x110: (wumpa, wireless)}
    for at, (old, _new) in links.items():
        if struct.unpack_from('<Q', data, base + at)[0] != old:
            raise ValueError('Octane_MainMenu.igz: unexpected navigation')
    for at, (_old, new) in links.items():
        struct.pack_into('<Q', data, base + at, new)
    # One row (105) below Wumpa Challenges (30, 262.5), on the menu's slant.
    if struct.unpack_from('<2f', data, base + wireless + 0x60) != (12.0, 105.0):
        raise ValueError('Octane_MainMenu.igz: unexpected Wireless position')
    struct.pack_into('<2f', data, base + wireless + 0x60, 42.0, 367.5)
    # Its alpha is 0 (only the Switch layout fades it in), as the Graphics tab's was: now 1.
    if data[base + wireless + 0xe0:base + wireless + 0xe4] != b'\0' * 4:
        raise ValueError('Octane_MainMenu.igz: unexpected Wireless alpha')
    data[base + wireless + 0xe0:base + wireless + 0xe4] = struct.pack('<f', 1.0)
    # Its standard-layout animation keeps it at alpha 0 (keyframe type 0xd, value 0): now 1.
    key = 0x5dc8
    if (struct.unpack_from('<qqq', data, base + key) != (0xd, 1, -1) or
            data[base + key + 0x18:base + key + 0x1c] != b'\0' * 4):
        raise ValueError('Octane_MainMenu.igz: unexpected Wireless keyframe')
    data[base + key + 0x18:base + key + 0x1c] = struct.pack('<f', 1.0)
    return own_quit_level(bytes(data), wireless)


def own_quit_level(blob, wireless):
    """Only the Wireless button's level becomes the quit map (OCTR v0.7.1).

    The file keeps each string once: the Online button's script behavior and Wireless's both hold
    the index of the one "Octane/Octane_OnlineLobby/Octane_OnlineLobby". v0.7 renamed that string,
    so Online loaded the quit map and closed the game. Now a new string is added to the table and
    only Wireless's behavior (the igGuiVscBehavior in the placeable's behavior list) points to it;
    Online's still names its lobby."""
    import bisect
    sys.path.insert(0, str(Path(__file__).resolve().parent))  # scripts/ (also with python -I)
    from igz_file import Igz
    igz = Igz(blob)
    online = QUIT_LEVEL[0][:-1].decode()
    if igz.strings.count(online) != 1:
        raise ValueError('Octane_MainMenu.igz: online lobby level not found')
    k = igz.strings.index(online)
    objects = igz.objects()
    starts = [o for o, _ in objects]

    def owner(off):
        return objects[bisect.bisect_right(starts, off) - 1]

    def referrers(target):
        return [s for s in igz.sites['ROFS'] if igz.u64(s) & 0x0FFFFFFFFFFFFFFF == target]

    mine, others = [], 0
    for site in igz.sites['RSTT']:
        if igz.u64(site) != k:
            continue
        behavior, kind = owner(site)
        lists = [owner(s)[0] for s in referrers(behavior) if owner(s)[1] == 'igGuiBehaviorList']
        placeables = {owner(s)[0] for lst in lists for s in referrers(lst)
                      if owner(s)[1] == 'igGuiPlaceable'}
        if kind == 'igGuiVscBehavior' and placeables == {wireless}:
            mine.append(site)
        else:
            others += 1
    if len(mine) != 1 or others < 1:
        raise ValueError(f'Octane_MainMenu.igz: Wireless level references {len(mine)}, others {others}')
    igz.set_u64(mine[0], igz.string_index(QUIT_LEVEL[1][:-1].decode()))
    out = igz.build()
    check = Igz(out)
    if (check.strings[check.u64(mine[0])] != QUIT_LEVEL[1][:-1].decode() or
            sum(check.u64(s) == k for s in check.sites['RSTT']) != others):
        raise ValueError('Octane_MainMenu.igz: Wireless level not separated')
    return out


# Optional: built when found (an older game without the 1.21 menu keeps the rest).
OPTIONAL = {f'ui/octane_mainmenu_{lang}.lng': quit_labels(*labels)
            for lang, labels in QUIT_LABELS.items()}
OPTIONAL['ui/octane_mainmenu.igz'] = show_quit
# The main menu's root script handles Wireless by name (hidden off the Switch): the name matches
# nothing now, as with the Graphics tab.
OPTIONAL['vsc/ui/octane_mainmenu/octane_mainmenu_root.igz'] = rename(
    b'Comp__Menu_ButtonWithIcon', b'Comp__Menu_ButtonWithIcoX')


def build(game, dest, dump):
    with ExitStack() as stack:
        found = {}  # logical name (lower case) -> (archive, index, archive name)
        archives = []
        for name in ARCHIVES:
            path = game / 'archives' / name
            if not path.is_file():
                continue
            archive = Archive(path)
            stack.callback(archive.f.close)
            archives.append(archive)
            for i in range(len(archive.names)):
                logical = archive.logical(i)
                low = logical.lower()
                if any(d in low for d in DUMP):
                    target = dump / name / logical
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(archive.read(i))
                if (low in CHANGES or low in OPTIONAL) and low not in found:
                    found[low] = (archive, i, name)
        # Each edition ships a different language set; the graph/scripts and English are required.
        required = {name for name in CHANGES if not name.endswith('.lng')}
        required.add('ui/octane_options_menu_en.lng')
        if not required <= set(found):
            missing = ', '.join(sorted(required - set(found)))
            raise ValueError(f'not found: {missing}')
        files = []
        for low in sorted(set(CHANGES) & set(found)):
            archive, i, name = found[low]
            full, logical = archive.names[i]
            files.append((full, logical, archive.infos[i][1] & ~0xFF, CHANGES[low](archive.read(i))))
            print(f'Graphics tab: {logical} from {name}')
        # Quit is all-or-nothing for the languages present in this edition, after the Graphics files.
        quit_required = {name for name in OPTIONAL if not name.endswith('.lng')}
        quit_required.update(f'ui/octane_mainmenu_{lang}.lng' for lang in LABELS
                             if f'ui/octane_options_menu_{lang}.lng' in found)
        if quit_required <= set(found):
            try:
                quit_files = []
                for low in sorted(quit_required):
                    archive, i, _name = found[low]
                    full, logical = archive.names[i]
                    quit_files.append((full, logical, archive.infos[i][1] & ~0xFF,
                                       OPTIONAL[low](archive.read(i))))
                files += quit_files
                print('Graphics tab: Quit Game button in the main menu')
            except (ValueError, KeyError, IndexError, struct.error, ImportError) as error:
                print(f'Graphics tab: no Quit Game button ({type(error).__name__}: {error})')
        dest.parent.mkdir(parents=True, exist_ok=True)
        write_archive(dest, files, archives[0])


def enabled(settings):
    if not settings or not Path(settings).is_file():
        return True
    for line in Path(settings).read_text(encoding='utf-8', errors='replace').splitlines():
        key, _, value = line.partition('=')
        if key.strip() == 'graphics_tab':
            return value.strip() != '0'
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--settings', type=Path)
    args = parser.parse_args()
    if not enabled(args.settings):
        return
    if (args.game / OUTPUT).exists():
        print(f'Graphics tab: the game already has {OUTPUT}, off')
        return
    stamp = '-'.join(f'{(args.game / "archives" / a).stat().st_size}'
                     for a in ARCHIVES if (args.game / 'archives' / a).is_file())
    folder = args.out / 'graphics-tab'
    dest = folder / f'update3-v{VERSION}-{stamp}.pak'
    try:
        if not dest.is_file():
            for old in folder.glob('*.pak'):
                old.unlink()
            build(args.game, dest, folder / 'dump')
    except (OSError, ValueError, lzma.LZMAError, zlib.error) as error:
        print(f'Graphics tab: {error}, off')
        return
    print(f'Graphics tab: Options menu tab relabelled ({dest.name})')
    print(f'{OUTPUT}={dest}')


if __name__ == '__main__':
    main()
