"""Online item-box rules from the RyuNET server: the power-up odds and cooldowns, applied to a copy
of the game's looseData/systemData.igz (the newest copy is in update2.pak).

CPowerUpData (one object) holds the item tables: +0x10 by game mode offline, +0x18 by game mode
online, +0x20 the cooldowns, +0x88 the items' names. A game-mode table (an igHashTable keyed by
EOctaneRaceModes) gives a list of weight tables (EPowerUpType -> float weight): online single
races have eight, the racer's position 1st..8th; the battle modes share one; Steal the Bacon has
its own. An item's chance is its weight over the sum of the table's weights; 0 never gives it.

Rules (the server's /ryunet/theme "items", which run.py saves to a file named by BB_ITEM_RULES;
only the server sets them, since each player draws their own items):

    {"race": [{"Bomb": 8, "Missile": 0, ...}, ... up to 8 positions],
     "battle": {"Bomb": 311, ...},          # online battle modes (Limit, CTF, Last Kart, Crystal)
     "bacon": {"Bomb": 10, ...},            # online Steal the Bacon
     "cooldowns": {"N'Tropy Clock": 40},    # seconds between two of that item, any player
     "writes": [{"sha256": "<the entry's>", "offset": 1234, "f32": 1.5}]}

Items go by the game's own names (read_rules lists them). Values are written in place, so the
file keeps its size and layout. An item a table does not hold (the Super Engine in a race) goes
in one of its empty slots, experimentally: the game's draw walks the slots, but the table's hash
is unknown, so a lookup of that item by key would not find it.
The battle table and the cooldowns are shared with offline play, which they change too. "writes"
set raw values (f32 or u32) at offsets of the entry's data section, only for the exact entry
(its SHA-256): later findings (an item's duration...) can be served without a port update.

The patch for update2.pak keeps the other features' patch for it (scripts/archive_patch.py); the
archive itself is never changed.
"""
import hashlib
import json
import struct
from pathlib import Path

import archive_patch
from archive_reader import Archive
from igz_file import Igz
from mod_file_paths import resolve

ARCHIVE = 'update2.pak'
ENTRY = 'loosedata/systemdata.igz'
OFFLINE, ONLINE, COOLDOWNS, NAMES = 0x10, 0x18, 0x20, 0x88  # CPowerUpData
SINGLE_RACE, STEAL_THE_BACON, BATTLE = 1, 12, 9             # EOctaneRaceModes
POSITIONS = 8
EMPTY = 0xFFFFFFFF


class Tables:
    """The power-up tables of one systemData.igz (its data section, read in place)."""

    def __init__(self, blob):
        self.igz = Igz(blob)
        self.base = self.igz.sections[1][1]  # the data section's offset in the file
        found = [off for off, kind in self.igz.objects() if kind == 'CPowerUpData']
        if len(found) != 1:
            raise ValueError('systemData.igz: no single CPowerUpData')
        self.data = found[0]
        self.pointers = set(self.igz.sites['ROFS'])
        self.text = set(self.igz.sites['RSTT'])
        names = self.hash_table(self.ref(self.data + NAMES), 8)
        self.names = {key: self.igz.strings[self.igz.u64(at)] for key, at in names.items() if at in self.text}
        self.keys = {name: key for key, name in self.names.items()}

    def ref(self, at):
        if at not in self.pointers:
            raise ValueError(f'systemData.igz: no reference at {at:#x}')
        return self.igz.u64(at)

    def hash_table(self, table, step=None):
        """{key: offset of its value} of the igHashTable at table."""
        values, keys = self.ref(table + 0x18), self.ref(table + 0x28)
        count = self.igz.u32(table + 0x20) // 4
        step = step or self.igz.u32(table + 0x10) // count
        out = {}
        for i in range(count):
            key = self.igz.u32(keys + 4 * i)
            if key != EMPTY:
                out[key] = values + step * i
        return out

    def weight_lists(self):
        """{mode: [weight table offsets]} of the online table."""
        out = {}
        for mode, at in self.hash_table(self.ref(self.data + ONLINE), 8).items():
            if at not in self.pointers:
                continue
            lst = self.igz.u64(at)
            items = self.ref(lst + 0x20)
            out[mode] = [self.ref(items + 8 * i) for i in range(self.igz.u32(lst + 0x10))]
        return out

    def weights(self, table):
        """{item name: offset of its float} of a CPowerUpWeightHashTable."""
        return {self.names.get(k, str(k)): at for k, at in self.hash_table(table, 4).items()}

    def free_slot(self, table, taken=()):
        """(key offset, value offset) of an empty slot of a CPowerUpWeightHashTable, or None."""
        values, keys = self.ref(table + 0x18), self.ref(table + 0x28)
        for i in range(self.igz.u32(table + 0x20) // 4):
            if self.igz.u32(keys + 4 * i) == EMPTY and keys + 4 * i not in taken:
                return keys + 4 * i, values + 4 * i
        return None

    def cooldowns(self):
        return {self.names.get(k, str(k)): at for k, at in self.hash_table(self.ref(self.data + COOLDOWNS), 4).items()}

    def f32(self, at):
        return struct.unpack_from('<f', self.igz.data, at)[0]

    def sections(self):
        """{rules section: [weight tables]} for the rules' race, battle and bacon."""
        lists = self.weight_lists()
        if len(lists.get(SINGLE_RACE, [])) != POSITIONS:
            raise ValueError('systemData.igz: the online race has no table per position')
        return {'race': lists[SINGLE_RACE], 'battle': lists.get(BATTLE, [])[:1],
                'bacon': lists.get(STEAL_THE_BACON, [])[:1]}


def entry_of(archive):
    entry = next((f for f in archive.files if f['path'].lower() == ENTRY), None)
    if entry is None:
        raise ValueError(f'{ARCHIVE} has no {ENTRY}')
    return entry


def read_rules(blob):
    """The rules the entry holds: the game's own odds (the server's "Original" preset)."""
    t = Tables(blob)
    rules = {}
    for section, tables in t.sections().items():
        values = [{name: round(t.f32(at), 4) for name, at in sorted(t.weights(table).items())} for table in tables]
        rules[section] = values if section == 'race' else (values[0] if values else {})
    rules['cooldowns'] = {name: round(t.f32(at), 4) for name, at in sorted(t.cooldowns().items())}
    rules['sha256'] = hashlib.sha256(blob).hexdigest()
    return rules


def number(value):
    value = float(value)
    if not value == value or value < 0 or value > 1e6:  # NaN, negative or absurd
        raise ValueError(f'{value} is not a weight')
    return value


def apply(blob, rules):
    """(the entry with the rules, [messages]). Values the entry cannot take are reported, not fatal."""
    t = Tables(blob)
    data = bytearray(blob)
    notes = []

    def put(at, value, fmt='<f'):
        struct.pack_into(fmt, data, t.base + at, value)

    for section, tables in t.sections().items():
        wanted = rules.get(section)
        if wanted is None:
            continue
        wanted = wanted if section == 'race' else [wanted]
        if not isinstance(wanted, list) or len(wanted) > len(tables):
            raise ValueError(f'items: "{section}" has {len(wanted)} tables, the game {len(tables)}')
        for position, (table, values) in enumerate(zip(tables, wanted), 1):
            slots = t.weights(table)
            where = f'position {position}' if section == 'race' else section
            added = []
            for name, value in (values or {}).items():
                if name in slots:
                    put(slots[name], number(value))
                    continue
                weight = number(value)
                if weight == 0:
                    continue  # nothing to add
                # An item the table lacks goes in an empty slot (experimental: the draw walks
                # every slot; a lookup by key, if the game made one, would not find it there).
                slot = t.free_slot(table, added) if name in t.keys else None
                if slot is None:
                    notes.append(f'{name} cannot be added to the {where} table')
                    continue
                key_at, value_at = slot
                put(key_at, t.keys[name], '<I')
                put(value_at, weight)
                added.append(key_at)
            if added:
                count = t.igz.u32(table + 0x30) + len(added)
                put(table + 0x30, count, '<I')
                notes.append(f'{len(added)} item(s) added to the {where} table (experimental)')
    slots = t.cooldowns()
    for name, value in (rules.get('cooldowns') or {}).items():
        if name in slots:
            put(slots[name], number(value))
        else:
            notes.append(f'{name} has no cooldown in this game')
    writes = rules.get('writes') or []
    if writes:
        digest = hashlib.sha256(blob).hexdigest()
        size = len(t.igz.data)
        for write in writes:
            if write.get('sha256') != digest:
                notes.append(f'write at {write.get("offset")} made for another systemData.igz: skipped')
                continue
            offset = int(write['offset'])
            if not 0 <= offset <= size - 4 or offset % 4:
                raise ValueError(f'items: write offset {offset} outside the data')
            if 'f32' in write:
                put(offset, float(write['f32']))
            else:
                put(offset, int(write['u32']) & 0xFFFFFFFF, '<I')
    return bytes(data), notes


def prepare(game, out, rules, base, log=print):
    """{guest archive: patch file} for update2.pak with the rules (empty without rules), keeping the
    patch base ({guest: patch file}) has for it. Kept between launches while the archive, the
    rules and the base patch are the same."""
    if not rules:
        return {}
    path = resolve(Path(game) / 'archives' / ARCHIVE)
    if not path.is_file():
        return {}
    guest = f'/app0/archives/{ARCHIVE}'
    folder = Path(out) / 'item_rules'
    target = folder / 'update2.bbpatch'
    base_patch = base.get(guest)
    stamp = {'archive': [path.stat().st_size, path.stat().st_mtime_ns],
             'rules': hashlib.sha256(json.dumps(rules, sort_keys=True).encode()).hexdigest(),
             'base': hashlib.sha256(Path(base_patch).read_bytes()).hexdigest() if base_patch else None}
    stamp_file = folder / 'stamp.json'
    try:
        if json.loads(stamp_file.read_text(encoding='utf-8')) == stamp and target.is_file():
            return {guest: target}
    except (OSError, ValueError):
        pass
    archive = Archive(path)
    entry = entry_of(archive)
    changed, notes = apply(bytes(archive.extract(entry)), rules)
    for note in notes:
        log(f'Item rules: {note}')
    folder.mkdir(parents=True, exist_ok=True)
    archive_patch.write_entries_patch(target, path, guest, {entry['index']: changed}, base_patch)
    stamp_file.write_text(json.dumps(stamp), encoding='utf-8')
    return {guest: target}


def main():
    import argparse
    import sys
    p = argparse.ArgumentParser(description='Print the game\'s item rules (the "Original" preset) as JSON.')
    p.add_argument('game')
    args = p.parse_args()
    archive = Archive(resolve(Path(args.game) / 'archives' / ARCHIVE))
    json.dump(read_rules(bytes(archive.extract(entry_of(archive)))), sys.stdout, indent=1, ensure_ascii=False)


if __name__ == '__main__':
    main()
