"""Native menu adapter checks; BB_TEST_GAME enables tests using a local game dump."""
from paths import ROOT
import os
import struct
import subprocess
from pathlib import Path
import unittest
import tempfile
from unittest.mock import patch

import gcn_shaders as iga
from igz_file import Igz, unpack_offsets
import online_mode
from prepare import parse_self


class OnlineModeTests(unittest.TestCase):
    def test_linux_launch_prepares_both_regions(self):
        script = (ROOT / 'run.sh').read_text()
        block = script[script.index('unset BB_ONLINE_MODE_HOOK BB_SPEED_MODE'):
                       script.index('# The game runs at its own 30 FPS')]
        setup = '''
set -eu
prepare_test() {
    case "$1" in
        scripts/online_mode.py)
            echo 'archive_patches=menu.bbpatch'
            echo 'online_mode_hook=test-hook' ;;
        scripts/speed_mode.py)
            echo 'archive_patches=normal.bbpatch'
            echo 'private_archive_patches=speed.bbpatch' ;;
        *) return 1 ;;
    esac
}
PYTHON=prepare_test
game=game
out=out
title_id=$1
app_version=$2
BB_ONLINE=$3
BB_SPEED_SOLO_TEST=0
unset BB_ARCHIVE_PATCHES BB_PRIVATE_ARCHIVE_PATCHES
'''
        for title, version, online, enabled in (
                ('CUSA13795', '01.21', '1', True), ('CUSA14876', '01.21', '1', True),
                ('CUSA14876', '01.00', '1', False), ('CUSA14876', '01.21', '0', False)):
            with self.subTest(title=title, version=version, online=online):
                result = subprocess.run(
                    ['bash', '-c', setup + block +
                     '\nprintf "%s|%s|%s" "${BB_ONLINE_MODE_HOOK:-}" '
                     '"${BB_ARCHIVE_PATCHES:-}" "${BB_PRIVATE_ARCHIVE_PATCHES:-}"',
                     'test', title, version, online], capture_output=True, text=True, check=True)
                self.assertEqual(result.stdout,
                                 'test-hook|normal.bbpatch|speed.bbpatch' if enabled else '||')

    def test_unknown_graph_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'supported USA'):
            online_mode.rebuild(b'not a supported menu')

    def test_hook_requires_supported_adapter(self):
        with self.assertRaisesRegex(ValueError, 'not uniquely recognized'):
            online_mode.hook_site(bytes(512))

    def test_unknown_cup_graph_is_rejected(self):
        for specification in online_mode.CUP_GRAPHS.values():
            with self.assertRaisesRegex(ValueError, 'unsupported native CUP'):
                online_mode.cup_route(b'unknown graph', specification)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME') and os.name == 'posix', 'requires a local game dump and executable mmap')
    def test_native_cup_podium_sort(self):
        import ctypes
        import mmap
        elf, _, headers, *_ = parse_self((Path(os.environ['BB_TEST_GAME']) / 'eboot.bin').read_bytes())
        shift = 0x1277e00 - online_mode.hook_site(elf)
        start = 0xeac6c0 - shift
        segment = next(p for p in headers if p['type'] == 1 and p['vaddr'] <= start < p['vaddr'] + p['filesz'])
        offset = segment['offset'] + start - segment['vaddr']
        # Run the actual eight-racer native sort and comparator. Only external
        # memmove and the human-player predicate use isolated test substitutes.
        with mmap.mmap(-1, 0x2000, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC) as code:
            code[:0x6d0] = elf[offset:offset + 0x6d0]
            move = ctypes.cast(ctypes.CDLL(None).memmove, ctypes.c_void_p).value
            code[0x1000:0x100c] = b'\x48\xb8' + struct.pack('<Q', move) + b'\xff\xe0'
            code[0x1020:0x1028] = bytes.fromhex('0f b6 87 b0 00 00 00 c3')
            for call, target in [(0xeac990, 0x1000)] + [(a, 0x1020) for a in (0xeacd27, 0xeacd33, 0xeacd43, 0xeacd7f)]:
                pos = call - 0xeac6c0
                self.assertEqual(code[pos], 0xe8)
                code[pos + 1:pos + 5] = struct.pack('<i', target - pos - 5)
            address = ctypes.addressof(ctypes.c_char.from_buffer(code))
            sort = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t)(address)
            racers = [ctypes.create_string_buffer(0x180) for _ in range(8)]
            def order(points, humans, finishes):
                for i, racer in enumerate(racers):
                    struct.pack_into('<i', racer, 0xb8, points[i])
                    struct.pack_into('<i', racer, 0x60, finishes[i])
                    racer[0x8c] = b'\x01'
                    racer[0xb0] = bytes([i in humans])
                pointers = (ctypes.c_void_p * 8)(*[ctypes.addressof(r) for r in reversed(racers)])
                sort(ctypes.addressof(pointers), ctypes.addressof(pointers) + ctypes.sizeof(pointers), 8)
                lookup = {ctypes.addressof(r): i for i, r in enumerate(racers)}
                return [lookup[p] for p in pointers]
            # Hasty, n0buholic, Coco: final-track winner/third must not win/third CUP.
            self.assertEqual(order([25, 21, 12, 9, 4, 4, 1, 0], {1, 7}, [2, 1, 5, 3, 4, 6, 7, 8])[:3], [0, 1, 2])
            # Native ties prefer humans, then the race's finishing position.
            self.assertEqual(order([10] * 8, {1, 7}, [1, 3, 4, 5, 6, 7, 8, 2]), [7, 1, 0, 2, 3, 4, 5, 6])

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local USA/EU 1.21 game dump')
    def test_native_cup_navigation(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update.pak'
        entries = iga.pak_entries(pak)
        with pak.open('rb') as f:
            for name, spec in online_mode.CUP_GRAPHS.items():
                entry = next(e for e in entries if e[0].endswith(name + '.igz'))
                blob = iga.read_entry(f, *entry[1:])
                old, new = Igz(blob), Igz(online_mode.cup_route(blob, spec))
                _, node, project, entering = spec
                count = 11 if entering else 12 if node == 0x818 else 10
                self.assertEqual(new.u32(node + 0x10), count)
                before, after = old.u64(node + 0x20), new.u64(node + 0x20)
                self.assertEqual(new.data[after:after + 64], old.data[before:before + 64])
                self.assertEqual(len(new.objects()), len(old.objects()) + 1)
                names = new.fixup('EXNM')
                handles = [(new.strings[a], new.strings[b & 0x7fffffff])
                           for a, b in struct.iter_unpack('<II', names[3][:names[1] * 8])
                           if b & 0x80000000]
                for index, expected in ((8, (project, project)),):
                    pin = new.u64(after + index * 8)
                    self.assertIn(pin + 16, new.read_list('RHND', unpack_offsets))
                    self.assertEqual(handles[new.u32(pin + 16) & 0x7fffffff], expected)
                self.assertEqual([f for f in new.fixups if f[0] == 'TDEP'],
                                 [f for f in old.fixups if f[0] == 'TDEP'])
                for site in new.sites['ROFS']:
                    self.assertLess(new.u64(site), len(new.data))

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires native game dump')
    def test_cup_streaming_package(self):
        game = Path(os.environ['BB_TEST_GAME'])
        pak = game / 'archives/update2.pak'
        entries = {e[0].lower(): e for e in iga.pak_entries(pak)}
        with pak.open('rb') as f:
            blob = iga.read_entry(f, *entries[online_mode.ONLINE_PACKAGE][1:])
            source = iga.read_entry(f, *entries[online_mode.START_PACKAGE][1:])
        old = Igz(blob)
        with tempfile.TemporaryDirectory() as temp:
            assets = Path(temp)
            new = Igz(online_mode.cup_package(game, blob, source, assets))
            archive = game / 'archives/juicedomain_startscreen.pak'
            entry = next(e for e in iga.pak_entries(archive)
                         if e[0].lower().endswith('/ui/octane_cupselect_en.lng'))
            with archive.open('rb') as f:
                self.assertEqual((assets / 'ui/octane_cupselect_en.lng').read_bytes(),
                                 iga.read_entry(f, *entry[1:]))
            self.assertFalse((assets / 'vsc/ui/octane_cupselect/octane_cupselect_listbutton.igz').exists())
        chunk, before = online_mode.package_rows(old)
        _, after = online_mode.package_rows(new)
        self.assertEqual(after[:len(before)], before)
        self.assertEqual(len(after), len(set(after)))
        added = set(after) - set(before)
        self.assertGreater(len(added), 40)
        self.assertLess(len(added), 70)
        for row in [('gui_project', 'ui/octane_cupselect.igz'),
                    ('lang_file', 'ui/octane_cupselect_en.lng'),
                    ('material_instances', 'materialinstances/ui/cupselect_materials.igz')]:
            self.assertIn(row, added)
        self.assertTrue(any('bg_wumpacup' in path for _, path in added))
        self.assertEqual(old.u64(chunk + 0x30) >> 32, new.u64(chunk + 0x30) >> 32)
        array = new.u64(chunk + 0x38)
        self.assertTrue(set(range(array, array + len(after) * 16, 8)) <= set(new.sites['RSTT']))

    def test_hook_regions_and_invalid_displacement(self):
        for address, displacement in ((0x1277e00, 0x187a197), (0x1277df0, 0x187a1a7)):
            code = bytes.fromhex('55 48 89 e5 41 57 41 56 53 50 48 8d 05')
            code += struct.pack('<i', displacement)
            with patch.object(online_mode, 'load_segments', return_value=[(address, 0, len(code))]):
                self.assertEqual(online_mode.hook_site(code), address)
                wrong = code[:13] + struct.pack('<i', displacement ^ 0x30)
                with self.assertRaises(ValueError):
                    online_mode.hook_site(wrong)
            with patch.object(online_mode, 'load_segments', return_value=[(address, 0, len(code))] * 2):
                with self.assertRaises(ValueError):
                    online_mode.hook_site(code)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local USA/EU 1.21 game dump')
    def test_real_selector_options_and_relocations(self):
        game = Path(os.environ['BB_TEST_GAME'])
        elf, _, headers, *_ = parse_self((game / 'eboot.bin').read_bytes())
        hook = online_mode.hook_site(elf)
        shift = 0x1277e00 - hook

        def at(address, size):
            segment = next(p for p in headers if p['type'] in (1, 0x61000010) and
                           p['vaddr'] <= address and address + size <= p['vaddr'] + p['filesz'])
            offset = segment['offset'] + address - segment['vaddr']
            return elf[offset:offset + size]

        self.assertEqual(struct.unpack('<Q', at(0x2773b40, 8))[0], 0x1277c70 - shift)
        self.assertEqual(struct.unpack('<Q', at(0x254c430, 8))[0], 0xa298c0)
        # Native host-results receiver writes the resolved finishing position.
        self.assertEqual(at(0xa29a84, 4), bytes.fromhex('45 89 6e 60'))
        # Podium-only branch follows the native race-position write (same USA/EU address).
        self.assertEqual(at(0xa29a92, 17), bytes.fromhex('48 83 7d b0 00 74 06 41 83 fd 03 7e 21 4c 8b 6d 90'))
        self.assertEqual(at(0xa2a386, 6), bytes.fromhex('0f 85 0d f7 ff ff'))
        self.assertEqual(at(0xa2a73a, 6), bytes.fromhex('0f 85 59 f3 ff ff'))
        self.assertEqual(at(0xeac6c0 - shift, 8), bytes.fromhex('55 48 89 e5 41 57 41 56'))
        # The online podium gate reads current-zone data +0x1b0, pending flag +0x71.
        self.assertEqual(at(0xf51465 - shift, 7), bytes.fromhex('48 8b 80 b0 01 00 00'))
        self.assertEqual(at(0xf51475 - shift, 3), bytes.fromhex('8a 40 71'))
        # Native CUP comparator prioritizes cumulative points, then native tie rules.
        self.assertEqual(at(0xeacd0d - shift, 13), bytes.fromhex('41 8b 86 b8 00 00 00 39 83 b8 00 00 00'))
        # CUP track 2+ places racers from previous finish +0x60, before physics.
        self.assertEqual(at(0xf31160 - shift, 17), bytes.fromhex('55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58'))
        self.assertEqual(at(0xf31813 - shift, 6), bytes.fromhex('41 8b 46 60 eb ce'))
        self.assertEqual(at(0xe9ba30 - shift, 17), bytes.fromhex('55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 48'))
        self.assertEqual(struct.unpack('<Q', at(0x2432338, 8))[0], 0x253740)
        self.assertEqual(at(0x253740, 9), bytes.fromhex('48 8b 7f 10 e9 c7 ad ff ff'))
        self.assertEqual(at(0x986820, 17), bytes.fromhex('55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 48'))
        self.assertEqual(at(0x252110, 8), bytes.fromhex('8b 47 0c b9 0a 01 00 00'))
        # Pool reset is at the same address in USA and EU.
        self.assertEqual(at(0xe6f830, 17), bytes.fromhex('55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18'))
        self.assertEqual(at(0xfff740 - shift, 20), bytes.fromhex('55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec d8 00 00 00'))
        # Active-count equality skips driver loading. Cleanup must clear active
        # participants while retaining selections; bypass cached CUP resources.
        self.assertEqual(at(0xffd7a2 - shift, 16), bytes.fromhex('48 8b 4f 38 48 8b 47 30 8b 49 0c 3b 48 0c 75 26'))
        self.assertEqual(at(0xffd9c3 - shift, 10), bytes.fromhex('80 7d 97 00 0f 85 f2 0b 00 00'))
        # Intermediate Arcade CUP reuses old racer records; online has cleared them.
        self.assertEqual(at(0xfff9bb - shift, 13), bytes.fromhex('80 bd 4f ff ff ff 00 0f 85 d6 06 00 00'))
        # Fallthrough uses native lookup-or-create, preserving native initialization.
        for site, target in ((0xfffa08, 0xfd4230), (0xfffa1d, 0xff5440)):
            call = at(site - shift, 5)
            self.assertEqual(call[0], 0xe8)
            self.assertEqual(site - shift + 5 + struct.unpack_from('<i', call, 1)[0], target - shift)
        self.assertEqual(at(0xe73820 - shift, 17), bytes.fromhex('55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18'))
        self.assertEqual(at(0xe73460 - shift, 10), bytes.fromhex('55 48 89 e5 41 57 41 56 53 50'))
        self.assertEqual(struct.unpack('<Q', at(0x2773b50, 8))[0], 0x97dfc0)
        self.assertEqual(struct.unpack('<Q', at(0x27744a8, 8))[0], 0x127c6b0 - shift)
        self.assertEqual(struct.unpack('<Q', at(0x2777738, 8))[0], 0x128e320 - shift)
        self.assertEqual(struct.unpack('<Q', at(0x2777748, 8))[0], 0x128e3a0 - shift)
        self.assertEqual(struct.unpack('<Q', at(0x2778390, 8))[0], 0x1293ce0 - shift)
        self.assertEqual(struct.unpack('<Q', at(0x2543218, 8))[0], 0x9b4940)
        self.assertEqual(at(0xfb7710 - shift, 15),
                         bytes.fromhex('55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 20'))
        pin_read = bytearray.fromhex('49 8b 46 10 48 8b 30 e8 63 76 36 ff 48 c7 45 c0 00 00 00 00')
        pin_read[8] += shift
        self.assertEqual(at(0x1109231 - shift, 20), pin_read)
        self.assertEqual(at(0xfdeb70 - shift, 13), bytes.fromhex('55 48 89 e5 41 56 53 48 89 fb 83 fe 06'))
        for address in (0x125d570 - shift, 0x125da80 - shift, 0xf50350 - shift):
            self.assertEqual(at(address, 8), bytes.fromhex('55 48 89 e5 41 57 41 56'))
        self.assertEqual(at(0x9b1fc0, 14), bytes.fromhex('55 48 89 e5 41 56 53 48 81 ec 30 01 00 00'))
        self.assertEqual(at(0xe6675b, 17),
                         bytes.fromhex('41 83 fd 01 0f 85 e9 19 00 00 48 8b 35') +
                         struct.pack('<i', 0x1c5a654))
        self.assertEqual(at(0xf34573 - shift, 5), bytes.fromhex('45 85 ed 7e c7'))
        self.assertEqual(at(0xfff160 - shift, 14), bytes.fromhex('55 48 89 e5 41 57 41 56 41 54 53 49 89 fe'))
        self.assertEqual(at(0xf34437 - shift, 7), bytes.fromhex('4c 8b 40 10 4d 85 c0'))
        self.assertEqual(at(0xe9ba00 - shift, 9), bytes.fromhex('48 8b 78 28 e8 c7 c4 65 00'))
        self.assertEqual(at(0xff2320 - shift, 8), bytes.fromhex('55 48 89 e5 41 57 41 56'))
        # R15D counts every finisher; R13D only the finished human players.
        self.assertEqual(at(0xf344db - shift, 3), bytes.fromhex('45 01 e7'))
        # The native next-track function forwards its incoming zone (RDI) to
        # the index lookup; the lookup compares that object with zone handles.
        self.assertEqual(at(0xfdde28 - shift, 3), bytes.fromhex('49 89 fe'))
        self.assertEqual(at(0xfddebe - shift, 5), bytes.fromhex('4d 39 f4 75 19'))
        call = at(0x1046256 - shift, 5)
        self.assertEqual(call[0], 0xe8)
        self.assertEqual(0x104625b - shift + struct.unpack_from('<i', call, 1)[0],
                         0xfdde10 - shift)
        timer = at(0xeac310 - shift, 32)
        self.assertEqual(timer[:5], bytes.fromhex('55 48 89 e5 e8'))
        for offset, end, target in ((5, 9, 0x94efa0), (16, 20, 0x9fa130), (26, 30, 0xa9cbe0)):
            self.assertEqual(0xeac310 - shift + end + struct.unpack_from('<i', timer, offset)[0], target)
        self.assertEqual(at(0xeb9b60 - shift, 17),
                         bytes.fromhex('55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18'))
        self.assertEqual(at(0xeb9ffb - shift, 15),
                         bytes.fromhex('49 8b 06 4c 89 f7 4c 89 fe ff 90 b0 00 00 00'))
        self.assertEqual(at(0xf4f7c0 - shift, 11),
                         bytes.fromhex('55 48 89 e5 41 57 41 56 41 54 53'))
        self.assertEqual(at(0xebe451 - shift, 17),
                         bytes.fromhex('48 8b 05') + struct.pack('<i', 0x1c0f9f0 + shift) +
                         bytes.fromhex('8b 80 40 0d 00 00 41 89 46 3c'))
        self.assertEqual(at(0xf4f972 - shift, 14),
                         bytes.fromhex('4c 89 e7 8b 80 40 0d 00 00 41 89 44 24 3c'))
        pak = game / 'archives/update2.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower() == online_mode.ENTRY)
        with pak.open('rb') as f:
            original = iga.read_entry(f, *entry[1:])
            card_entry = next(e for e in iga.pak_entries(pak) if e[0].lower() == online_mode.LOBBY_ENTRY)
            card_blob = iga.read_entry(f, *card_entry[1:])
        card_old, card_new = Igz(card_blob), Igz(online_mode.lobby_card(card_blob))
        self.assertEqual(card_old.objects(), card_new.objects())
        self.assertEqual(card_new.string_at(0x480 + 0x28), 'OCTR_PRIVATE_LOBBY')
        def behavior_order(asset):
            behaviors = asset.u64(0x480 + 0xe8)
            items = asset.u64(behaviors + 0x20)
            return [asset.u64(items + 8 * i) for i in range(asset.u32(behaviors + 12))]
        before, after = behavior_order(card_old), behavior_order(card_new)
        self.assertCountEqual(before, after)
        self.assertEqual(dict(card_new.objects())[after[-1]],
                         'CGuiBehaviorReturnToMainMenuOnDisconnect')
        self.assertEqual([v for v in before if v != after[-1]], after[:-1])
        self.assertEqual(card_new.string_at(0x4180 + 0x28), 'OCTR_PRIVATE_PREVIEW')
        old, new = Igz(original), Igz(online_mode.rebuild(original))
        self.assertEqual(len(new.objects()), len(old.objects()) + 8)
        row = new.u64(0x1058)
        self.assertEqual(new.u64(row + 0x40), 0xea0)
        self.assertEqual(new.u64(row + 0x50), 0x1000)
        self.assertEqual(new.u64(row + 0x58), 0x1120)
        self.assertEqual(new.u64(0x1170), row)
        y = lambda off: struct.unpack_from('<f', new.data, off + 0x64)[0]
        self.assertEqual(y(row) - y(0x1000), y(0x1000) - y(0xb10))
        behavior_list = new.u64(row + 0xe8)
        behavior = new.u64(new.u64(behavior_list + 0x20))
        self.assertEqual(new.string_at(behavior + 0x148), 'RACE TYPE')
        race_options = new.u64(behavior + 0x1a8)
        self.assertEqual(new.u32(race_options + 12), 2)
        self.assertEqual([new.string_at(new.u64(race_options + 32) + i * 8) for i in range(2)],
                         ['SINGLE', 'CUP'])
        self.assertNotEqual(new.u64(behavior + 0x58), old.u64(0x1678 + 0x58))
        self.assertIn(row + 0x38, new.read_list('RHND', unpack_offsets))
        self.assertIn(behavior + 0x150, new.read_list('RNEX', unpack_offsets))
        self.assertEqual(new.u32(0xfcc), 3)
        self.assertEqual([new.u64(new.u64(0xfe0) + i * 8) for i in range(3)],
                         [0x1000, row, 0x1120])
        self.assertEqual(new.u32(0x188c), 3)
        self.assertEqual(new.u32(0x18b4), 3)
        options = new.u64(0x18a0)
        self.assertEqual([new.string_at(options + i * 8) for i in range(3)],
                         ['NORMAL', 'ITEMLESS', 'BATTLE'])
        available = new.u64(0x18c8)
        self.assertEqual(new.data[available:available + 3], bytes((1, 1, 1)))
        for lst in (0x1880, 0x18a8):
            self.assertTrue(new.u64(lst + 0x18) & (1 << 56), 'archive data must not be heap-freed')
        for i in range(3):
            self.assertIn(options + i * 8, new.sites['RSTT'])
        for site in new.sites['ROFS']:
            self.assertLess(new.u64(site), len(new.data))
        pak = game / 'archives/update.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower() == online_mode.LABEL_ENTRY)
        with pak.open('rb') as f:
            label = Igz(iga.read_entry(f, *entry[1:]))
        timer_entry = next(e for e in iga.pak_entries(pak) if e[0].lower() == online_mode.TIMER_ENTRY)
        with pak.open('rb') as f:
            timer = Igz(online_mode.cup_timer(iga.read_entry(f, *timer_entry[1:])))
        self.assertEqual(timer.string_at(0xc0 + 0x28), 'OCTR_PRIVATE_RETURN_TIMER')
        # Without relocation native cloning treats the string index 0x13 as a
        # pointer and faults at 0xb while incrementing its reference count.
        self.assertIn(0xc0 + 0x28, timer.sites['RSTT'])
        self.assertEqual(timer.string_at(0x490 + 0x28), 'TimerDesc')
        label.strings[label.strings.index('Racing Type')] = 'GAME MODE'
        self.assertIn('GAME MODE', Igz(label.build()).strings)



if __name__ == '__main__':
    unittest.main()
