"""16:10 boost gauge proportions, made from the player's HUD assets.

The shared boost gauge keeps its bottom edge and corrects its vertical proportions
in every HUD that instantiates it. GUI VFX placement is corrected by the 16:10
executable patch; collectible assets and their animation keys remain authored.

The IGZ reader and charging-effect cleanup are from tcosi's existing layout work.
No game files are changed; all other asset bytes and existing archive overlays stay.
SPDX-License-Identifier: GPL-2.0-or-later
"""
import hashlib
import json
import lzma
import math
import struct
import zlib
from pathlib import Path

import archive_patch
from archive_reader import Archive
from nitro_components import sha
from widescreen_startup import IGZ

FORMAT = 1
FACTOR = .9  # (16 / 10) / (16 / 9), square HUD pixels at the selected output
ARCHIVES = ('update2.pak', 'update.pak', 'juicedomain_hub.pak', 'juicedomain_race.pak',
            'juicedomain_race_flareday.pak', 'juicedomain_race_flarenight.pak')


def _require(ok, message):
    if not ok:
        raise ValueError('Unsupported 16:10 HUD: ' + message)


def _close(values, expected):
    return len(values) == len(expected) and all(
        math.isfinite(v) and abs(v - e) <= .001 for v, e in zip(values, expected))


class _Asset:
    def __init__(self, data):
        self.data = bytes(data)
        self.ig = IGZ(self.data)
        self.objects = {o['ptr']: o for o in self.ig.objects}
        self.result = bytearray(self.data)
        self.writes = {}

    def obj(self, pointer, kind):
        obj = self.objects.get(pointer)
        _require(obj is not None and obj['type'] == kind, 'expected ' + kind)
        return obj

    def named(self, name):
        found = [o for o in self.ig.objects if o.get('name') == name]
        _require(len(found) == 1 and found[0]['type'] == 'igGuiPlaceable', name)
        return found[0]

    def q(self, pointer):
        return self.ig.q(self.ig.offset(pointer))

    def vector(self, obj, field, count=3):
        return struct.unpack_from('<' + str(count) + 'f', self.data, obj['offset'] + field)

    def expect(self, obj, field, expected):
        _require(_close(self.vector(obj, field, len(expected)), expected),
                 obj.get('name', obj['type']) + ' field ' + hex(field))

    def write(self, offset, value):
        _require(math.isfinite(value), 'non-finite result')
        if offset in self.writes:
            _require(abs(self.writes[offset] - value) < .001, 'incompatible shared keyframe')
        self.writes[offset] = value
        struct.pack_into('<f', self.result, offset, value)

    def check_transform(self, obj, xy, origin, scale, size, rotation=(0, 0, 0)):
        self.expect(obj, 0x60, xy)
        self.expect(obj, 0x6c, origin)
        self.expect(obj, 0x78, rotation)
        self.expect(obj, 0x84, scale)
        self.expect(obj, 0x90, size)
        _require(struct.unpack_from('<2I', self.data, obj['offset'] + 0xa0) == (0, 0),
                 obj['name'] + ' alignment')

    def parent(self, obj, expected):
        parent = self.obj(self.q(obj['ptr'] + 0x40), 'igGuiPlaceable')
        _require(parent.get('name') == expected, obj['name'] + ' parent')
        return parent

    def list_items(self, pointer, kind):
        obj = self.obj(pointer, kind)
        count = struct.unpack_from('<I', self.data, obj['offset'] + 12)[0]
        _require(count <= 256, kind + ' length')
        data = self.q(pointer + 0x20)
        return [self.q(data + i * 8) for i in range(count)]

    def tracks(self, owner):
        table = self.q(owner['ptr'] + 0xf0)
        if not table:
            return
        obj = self.obj(table, 'igGuiAnimationTable')
        capacity = struct.unpack_from('<I', self.data, obj['offset'] + 0x10)[0]
        _require(capacity <= 4096 and capacity % 8 == 0, 'animation table capacity')
        data = self.q(table + 0x18)
        seen = set()
        for i in range(capacity // 8):
            pointer = self.q(data + i * 8)
            if not pointer or pointer in seen:
                continue
            seen.add(pointer)
            animation = self.obj(pointer, 'igGuiAnimation')
            for track in self.list_items(self.q(animation['ptr'] + 0x18), 'igGuiTrackList'):
                _require(track in self.objects, 'animation track')
                yield self.objects[track]


def _boost(data):
    a = _Asset(data)
    root = a.named('Octane_NFBoostBar_asset_root')
    a.check_transform(root, (0, 0), (0, 0, .5), (1, 1, 1), (460, 160))
    master = a.named('Null_Asset_BarMaster')
    # This container's native center alignment is independent of the host HUD's scale.
    a.expect(master, 0x60, (0, 0))
    a.expect(master, 0x6c, (.5, .5, .5))
    a.expect(master, 0x78, (0, 0, 0))
    a.expect(master, 0x84, (1, 1, 1))
    a.expect(master, 0x90, (460, 160))
    a.parent(master, 'Octane_NFBoostBar_asset_root')
    _require(struct.unpack_from('<2I', data, master['offset'] + 0xa0) == (2, 2),
             'boost master alignment')
    for track in a.tracks(master):
        _require(track['type'] == 'igGuiVec3Track' and a.q(track['ptr'] + 0x18) == 10,
                 'boost master animation')
    height = a.vector(master, 0x90, 2)[1]
    origin_y = a.vector(master, 0x6c, 2)[1]
    a.write(master['offset'] + 0x64, height * (1 - FACTOR) * (1 - origin_y))
    a.write(master['offset'] + 0x88, FACTOR)
    charging = a.named('Effect_ctr_gui_boost_bar_charging_vfx')
    a.parent(charging, 'Null_Asset_BarMaster')
    a.expect(charging, 0x84, (1, 1, 1))
    a.expect(charging, 0x90, (10, 10))
    # Same named-field cleanup as the existing wide layouts.
    a.write(charging['offset'] + 0x88, .25)
    a.write(charging['offset'] + 0x94, 2.5)
    return bytes(a.result)


def _checked(transform, data):
    try:
        return transform(data)
    except (AssertionError, IndexError, KeyError, UnicodeError, struct.error) as error:
        raise ValueError('Unsupported 16:10 HUD asset') from error


def correct_boost_bar(data):
    return _checked(_boost, data)


def prepare(game, out, base):
    """Checked HUD archive overlays, composed with the launch's existing overlays.

    Without archives, callers can still prepare the executable-only launch plan.
    When archives are present, the boost gauge must be found and every affected
    copy must have the expected structure before the cached set is published.
    """
    folder = Path(game) / 'archives'
    sources = {name: folder / name for name in ARCHIVES if (folder / name).is_file()}
    if not sources:
        return {}
    stamp = {'format': FORMAT, 'code': sha(Path(__file__)),
             'archives': {name: [str(path.resolve()), path.stat().st_size, path.stat().st_mtime_ns]
                          for name, path in sources.items()},
             'base': {guest: sha(path) for guest, path in sorted(base.items())}}
    key = hashlib.sha256(json.dumps(stamp, sort_keys=True).encode()).hexdigest()[:16]
    target = Path(out) / 'aspect_ratio' / 'hud' / key
    try:
        cached = json.loads((target / 'stamp.json').read_text())
        expected = {'/app0/archives/' + name: name + '.bbpatch' for name in sources}
        if any(expected.get(guest) != name for guest, name in cached['patches'].items()):
            raise ValueError('Unexpected HUD cache entry')
        made = {guest: target / name for guest, name in cached['patches'].items()}
        if cached['source'] == stamp and made and all(path.is_file() for path in made.values()):
            return made
    except (OSError, KeyError, TypeError, ValueError):
        pass
    transforms = {'ui/guisharedassets/octane_nfboostbar.igz': correct_boost_bar}
    changed = {}
    found = set()
    for name, path in sources.items():
        archive = Archive(path)
        try:
            entries = {}
            for entry in archive.files:
                asset = entry['path'].lower()
                if asset in transforms:
                    entries[entry['index']] = transforms[asset](archive.extract(entry))
                    found.add(asset)
            if entries:
                changed[name] = entries
        except (AssertionError, IndexError, KeyError, UnicodeError, lzma.LZMAError,
                zlib.error, struct.error) as error:
            raise ValueError(name + ': unsupported 16:10 HUD archive') from error
        finally:
            archive.data.close()
            archive._file.close()
    if found != set(transforms):
        raise ValueError('16:10 HUD assets are missing: ' + ', '.join(sorted(set(transforms) - found)))
    target.mkdir(parents=True, exist_ok=True)
    made = {}
    for name, entries in changed.items():
        guest = '/app0/archives/' + name
        made[guest] = target / (name + '.bbpatch')
        archive_patch.write_entries_patch(made[guest], sources[name], guest, entries, base.get(guest))
    (target / 'stamp.json').write_text(json.dumps(
        {'source': stamp, 'patches': {guest: path.name for guest, path in made.items()}}, sort_keys=True))
    return made
