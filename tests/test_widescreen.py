"""The 21:9 and 32:9 layouts (scripts/widescreen.py): selection, variants, archive patches."""
import json
import struct
import sys
import xml.etree.ElementTree as ET
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import archive_patch
import nitro_format as fmt
import widescreen
from nitro_components import copy_delta
from patches import render_patches
from widescreen_online import correct_markers


def fake_archive(path, entries):
    """A minimal IGA v13 archive: the header, a hash table and one 16-byte record per entry."""
    count = len(entries)
    data = bytearray(0x38 + 20 * count)
    struct.pack_into('<II', data, 0, 0x1A414749, 13)
    struct.pack_into('<I', data, 0xc, count)
    for i, payload in enumerate(entries):
        offset = len(data)
        data += payload
        struct.pack_into('<4I', data, 0x38 + 4 * count + 16 * i, offset, 0x500, len(payload), 0xFFFFFFFF)
    Path(path).write_bytes(data)


def entry(archive, patch, index):
    """Entry index as the game reads it through the patch."""
    count, = struct.unpack_from('<I', archive_patch.view(archive, patch, 0, 0x38), 0xc)
    offset, extra, length, mode = struct.unpack('<4I', archive_patch.view(archive, patch, 0x38 + 4 * count + 16 * index, 16))
    assert mode == 0xFFFFFFFF
    return archive_patch.view(archive, patch, offset | (extra & 0xFF) << 32, length)


def fake_elf(code, at=0x80):
    """A minimal eboot.elf: one load segment (vaddr 0, file offset 0x100) with code at vaddr `at`."""
    elf = bytearray(0x200)
    struct.pack_into('<Q', elf, 0x20, 0x40)          # program headers
    struct.pack_into('<HH', elf, 0x36, 56, 1)
    struct.pack_into('<IIQQQQQQ', elf, 0x40, 1, 5, 0x100, 0, 0, 0x100, 0x100, 0x1000)
    elf[0x100 + at:0x100 + at + len(code)] = code
    return bytes(elf)


def signed_patch(signature, address=0x400080):
    xml = ET.Element('Patch')
    meta = ET.SubElement(xml, 'Metadata')
    ET.SubElement(ET.SubElement(meta, 'PatchList'), 'Line', Type='bytes', Address=hex(address), Value='90',
                  Signature=signature, SignatureOffset='0x0')
    return xml


class WidescreenTests(unittest.TestCase):
    def test_misfit_needs_every_line_in_its_one_place(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            self.assertIn('no eboot.elf', widescreen.misfit(signed_patch('de ad be ef'), out))
            (out / 'eboot.elf').write_bytes(fake_elf(b'\xde\xad\xbe\xef'))
            self.assertIsNone(widescreen.misfit(signed_patch('de ad ?? ef'), out))
            # Elsewhere in the executable (another build's addresses): still its one place.
            self.assertIsNone(widescreen.misfit(signed_patch('de ad be ef', 0x400010), out))
            self.assertIn('0 signature matches', widescreen.misfit(signed_patch('ca fe ba be'), out))
            (out / 'eboot.elf').write_bytes(fake_elf(b'\xde\xad\xbe\xef\x00\xde\xad\xbe\xef'))
            self.assertIn('2 signature matches', widescreen.misfit(signed_patch('de ad be ef', 0x400010), out))
            unsigned = signed_patch('de ad be ef')
            del next(unsigned.iter('Line')).attrib['Signature']
            self.assertIn('no signature', widescreen.misfit(unsigned, out))

    def test_executable_the_patches_do_not_fit_keeps_16x9(self):
        # The base game's executable under a 1.21 param.sfo: a signature found twice.
        for title in ('CUSA13795', 'CUSA14876'):
            with self.subTest(title=title), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                ini = root / 'bbport.ini'
                ini.write_text('widescreen_layout=21x9\n')
                with patch.object(widescreen, 'misfit', return_value='patch site not found (2 signature matches)'), \
                     patch.object(widescreen, 'archive_patches', return_value={}):
                    self.assertIsNone(widescreen.prepare(root / 'game', root / 'out', ini, {}, title, '01.21', {}))

    def test_alternate_executable_does_not_disable_layout(self):
        for mode, title in ((m, t) for m in widescreen.OUTPUTS for t in ('CUSA13795', 'CUSA14876')):
            with self.subTest(mode=mode, title=title), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                game = root / 'game'
                game.mkdir()
                (game / 'eboot.bin').write_bytes(b'alternate 1.21 executable')
                ini = root / 'bbport.ini'
                ini.write_text(f'widescreen_layout={mode}\nrender_res=1620\nonline=1\n')
                base = {'/app0/archives/update2.pak': root / 'playlist.bbpatch'}
                made = {'/app0/archives/update2.pak': root / 'combined.bbpatch'}
                original_sha = widescreen.sha

                def checked_sha(path):
                    self.assertNotEqual(Path(path).name, 'eboot.bin')
                    return original_sha(path)

                # Its patches fit it (misfit: every signed line in its one place).
                with patch.object(widescreen, 'sha', side_effect=checked_sha), \
                     patch('game_check.image_sha256', side_effect=AssertionError('Unexpected executable hash gate')), \
                     patch.object(widescreen, 'misfit', return_value=None), \
                     patch.object(widescreen, 'archive_patches', return_value=made) as assets:
                    plan = widescreen.prepare(game, root / 'out', ini, {}, title, '01.21', base)
                self.assertIsNotNone(plan)
                self.assertEqual(plan['archive_patches'], made)
                self.assertEqual(assets.call_args.args[4], base)
                lines = list(ET.parse(plan['patches_dir'] / 'Widescreen.xml').iter('Line'))
                self.assertEqual(len(lines), 15 if title == 'CUSA14876' else 16)
                self.assertTrue(all(line.get('Signature') for line in lines))

    def test_marker_hook_fits_every_variant_without_overlapping_writes(self):
        for mode in widescreen.OUTPUTS:
            folder = widescreen.DATA / ('ultrawide-' + mode)
            for variant, flat in fmt.expanded_variants(fmt.validate_folder(folder)):
                with self.subTest(mode=mode, variant=variant['id']):
                    factor = next(o['factor'] for o in flat['operations'] if o['kind'] == 'shader_position_scale')
                    xml = ET.parse(folder / flat['patches'][0]).getroot()
                    original = {int(l.get('Address'), 0): bytes.fromhex(l.get('Value')) for l in xml.iter('Line')}
                    correct_markers(xml, factor)
                    lines = {int(l.get('Address'), 0): bytes.fromhex(l.get('Value')) for l in xml.iter('Line')}
                    self.assertEqual(len(lines), 16)
                    spans = sorted((a, a + len(b)) for a, b in lines.items())
                    self.assertTrue(all(end <= next_start for (_, end), (next_start, _) in zip(spans, spans[1:])))
                    hook = lines[0xdd7853]
                    self.assertEqual(hook[0], 0xe8)
                    self.assertEqual(0xdd7858 + struct.unpack_from('<i', hook, 1)[0], 0x67d780)
                    blob = lines[0x67d4e5]
                    # Both viewport constants are stored at the end of the generated hook.
                    start = 0x67d780 - 0x67d4e5
                    self.assertEqual(blob[start:start + 4], bytes.fromhex('4883ec08'))
                    self.assertIn(struct.pack('<2f', .5, 1 / factor), blob[start:])
                    if factor != .5:
                        old = 0x67d7b0 - 0x67d4e5
                        self.assertEqual(blob[old + 32:old + 52], original[0x67d4e5][old:old + 20])

    def test_resized_asset_reuses_original_bytes(self):
        delta = {'format': 'copy-delta-v1', 'source_bytes': 6, 'result_bytes': 8,
                 'operations': [['copy', 0, 3], ['bytes', '7879'], ['copy', 3, 3]]}
        self.assertEqual(copy_delta(b'abcdef', delta), b'abcxydef')

    def test_copy_delta_rejects_bad_ranges_and_sizes(self):
        for operations, result_size in (([['copy', 4, 3]], 3), ([['copy', -1, 3]], 3), ([['copy', 0, 6]], 5),
                                        ([['copy', 0, 4]], 5), ([['unknown', 0, 4]], 4)):
            with self.subTest(operations=operations, result_size=result_size):
                with self.assertRaises(ValueError):
                    copy_delta(b'abcdef', {'format': 'copy-delta-v1', 'source_bytes': 6,
                                           'result_bytes': result_size, 'operations': operations})

    def test_selection(self):
        self.assertIsNone(widescreen.selection({}))
        self.assertIsNone(widescreen.selection({'widescreen_layout': 'off', 'ultrawide': '1'}))
        # The in-game ultrawide alone is not a layout: it keeps working as it did.
        self.assertIsNone(widescreen.selection({'ultrawide': '1', 'ultrawide_size': '3440x1440'}))
        self.assertEqual(widescreen.selection({'widescreen_layout': '21x9'}), ('21x9', '3440x1440', '1080'))
        self.assertEqual(widescreen.selection({'widescreen_layout': '32x9', 'widescreen_output': '3440x1440',
                                               'render_res': '900'}), ('32x9', '5120x1440', '1080'))
        self.assertEqual(widescreen.selection({'widescreen_layout': '32x9', 'widescreen_output': '7680x2160',
                                               'render_res': '1440'}), ('32x9', '7680x2160', '1440'))

    def test_every_output_and_internal_resolution_has_one_variant(self):
        for layout, outputs in widescreen.OUTPUTS.items():
            manifest = fmt.validate_folder(widescreen.DATA / ('ultrawide-' + layout))
            for output in outputs:
                for internal in widescreen.INTERNAL:
                    with self.subTest(layout=layout, output=output, internal=internal):
                        variant, flat = fmt.resolve(manifest, {'output_res': output, 'render_res': internal})
                        self.assertTrue(variant)
                        self.assertEqual(sum(o['kind'] == 'archive_asset' for o in flat['operations']), 224)
                        self.assertEqual(len(flat['patches']), 1)
                        self.assertEqual(widescreen.hud_scale(
                            [o for o in flat['operations'] if o['kind'].startswith('shader_')]).count(':'), 4)
                        # The variant sets the render size: the built-in render patches stay out.
                        effective = dict(flat['settings'], ultrawide='0')
                        self.assertEqual(render_patches(effective), [])

    def test_hud_scale(self):
        ops = [{'kind': 'shader_position_scale', 'target': 'cache/CUSA13795/0x00000000d4aaf527_0.spv', 'factor': 0.5}]
        self.assertEqual(widescreen.hud_scale(ops), 'd4aaf527:0:0.5')
        with self.assertRaises(ValueError):
            widescreen.hud_scale([dict(ops[0], kind='shader_file')])

    def test_not_used_for_16x9_or_other_editions(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ini = root / 'bbport.ini'
            for text, title, env in (('widescreen_layout=off\n', 'CUSA13795', {}),
                                     ('widescreen_layout=21x9\n', 'CUSA12345', {})):
                with self.subTest(text=text, title=title, env=env):
                    ini.write_text(text)
                    before = dict(env)
                    self.assertIsNone(widescreen.prepare(root / 'game', root / 'out', ini, env, title, '01.21', {}))
                    self.assertEqual(env, before)
                    self.assertFalse((root / 'out').exists())

    def test_online_keeps_selected_layout_and_playlist(self):
        for mode in widescreen.OUTPUTS:
            manifest = fmt.validate_folder(widescreen.DATA / ('ultrawide-' + mode))
            for online in ('0', '1'):
                for via_env in (False, True):
                    with self.subTest(mode=mode, online=online, via_env=via_env), tempfile.TemporaryDirectory() as tmp:
                        root = Path(tmp)
                        ini = root / 'bbport.ini'
                        ini.write_text(f'widescreen_layout={mode}\nonline={online}\n')
                        env = {'BB_ONLINE': online, 'BB_ONLINE_NAME': 'test'} if via_env else {}
                        before = dict(env)
                        base = {'/app0/archives/update2.pak': root / 'playlist.bbpatch'}
                        made = {'/app0/archives/update2.pak': root / 'combined.bbpatch'}
                        with patch.object(widescreen, 'sha', return_value=manifest['compatibility']['eboot_sha256'][0]), \
                             patch.object(widescreen, 'misfit', return_value=None), \
                             patch.object(widescreen, 'archive_patches', return_value=made) as assets:
                            result = widescreen.prepare(root / 'game', root / 'out', ini, env, 'CUSA13795', '01.21', base)
                        self.assertIsNotNone(result)
                        self.assertEqual(env, before)
                        self.assertIn(f'online={online}', result['config'].read_text())
                        self.assertEqual(result['archive_patches'], made)
                        self.assertEqual(assets.call_args.args[4], base)
                        self.assertIn('combined.bbpatch', widescreen.archive_list(base, result))
                        self.assertIn('BB_HUD_SCALE', result['env'])

    def test_eu_data(self):
        sets = json.loads((widescreen.EU / 'assets.json').read_text(encoding='utf-8'))['sets']
        self.assertEqual(sorted(sets), ['assets-2560', 'assets-32x9', 'assets-3440'])
        for name, ops in sets.items():
            with self.subTest(set=name):
                self.assertEqual(len(ops), 224)
                for op in ops:
                    self.assertEqual(op['kind'], 'archive_asset')
                    self.assertEqual(('payload' in op) + ('runs' in op), 1)
                    if 'payload' in op:  # a copy delta from the player's own asset, never a whole one
                        delta = json.loads((widescreen.EU / op['payload']).read_text(encoding='utf-8'))
                        self.assertEqual(delta['format'], 'copy-delta-v1')

    def test_every_executable_variant_ports_to_the_eu_edition(self):
        port = widescreen.eu_port()
        for path in sorted(widescreen.DATA.glob('ultrawide-*/patches/*.xml')):
            with self.subTest(path=path.name):
                for meta in ET.parse(path).getroot().iter('Metadata'):
                    eu = widescreen.port_meta(meta, port)
                    lines = list(eu.iter('Line'))
                    self.assertEqual(len(lines), 15)
                    addresses = {int(l.get('Address'), 0) for l in lines}
                    self.assertIn(0x17a9d61 - 16, addresses)  # the output size sites moved 16 bytes
                    for us, line in zip(meta.iter('Line'), lines):
                        self.assertEqual(len(line.get('Value')), len(us.get('Value')))
                        self.assertTrue(line.get('Signature'))

    def test_entries_patch_stacks_on_another_patch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pak = root / 'update2.pak'
            fake_archive(pak, [b'one', b'two', b'three'])
            guest = '/app0/archives/update2.pak'
            playlist = root / 'playlist.bbpatch'
            archive_patch.write_entry_patch(playlist, pak, guest, 1, b'TWO, longer')
            layout = root / 'layout.bbpatch'
            archive_patch.write_entries_patch(layout, pak, guest, {0: b'ONE!', 2: b'THREE' * 1000}, playlist)
            self.assertEqual([entry(pak, layout, i) for i in range(3)], [b'ONE!', b'TWO, longer', b'THREE' * 1000])
            self.assertEqual(pak.read_bytes()[-5:], b'three')  # the archive itself is as it was
            with self.assertRaises(ValueError):  # one entry, two replacements
                archive_patch.write_entries_patch(root / 'x.bbpatch', pak, guest, {1: b'2'}, playlist)
            with self.assertRaises(ValueError):  # a patch of another archive
                archive_patch.write_entries_patch(root / 'x.bbpatch', pak, '/app0/archives/update.pak', {0: b'1'}, playlist)

    def test_archive_list_replaces_the_base_patch_of_the_same_archive(self):
        base = {'/app0/archives/update2.pak': Path('playlist.bbpatch')}
        layout = {'archive_patches': {'/app0/archives/update2.pak': Path('a.bbpatch'),
                                      '/app0/archives/legal.pak': Path('b.bbpatch')}}
        names = [Path(p).name for p in widescreen.archive_list(base, layout).split('|')]
        self.assertEqual(sorted(names), ['a.bbpatch', 'b.bbpatch'])
        self.assertEqual([Path(p).name for p in widescreen.archive_list(base, None).split('|')], ['playlist.bbpatch'])
        self.assertEqual(widescreen.archive_list({}, None), '')


if __name__ == '__main__':
    unittest.main()
