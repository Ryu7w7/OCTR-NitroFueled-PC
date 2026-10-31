"""Aspect ratio selection and 16:10 patches; no game data or running engine needed."""
from paths import ROOT
import math
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch

import patches
import aspect_ratio
import widescreen


def projection_line():
    return ET.parse(aspect_ratio.DATA / '16x10.xml').find('./Metadata/PatchList/Line')


def game_code():
    signatures = [bytes.fromhex(patches.OUTPUT_SIZE_SIGNATURE),
                  *[bytes.fromhex(line.get('Signature')) for line in
                    ET.parse(aspect_ratio.DATA / '16x10.xml').iter('Line')],
                  bytes.fromhex(patches.RENDER_SCALE_SIGNATURE),
                  bytes.fromhex(patches.OUTPUT_DOUBLE_SIGNATURE),
                  bytes.fromhex(patches.GPU_HEAP_SIGNATURE)]
    return b''.join(bytes(64) + signature for signature in signatures) + bytes(64)


def game_elf(code):
    elf = bytearray(0x180) + code
    struct.pack_into('<Q', elf, 0x20, 0x40)
    struct.pack_into('<HH', elf, 0x36, 56, 1)
    struct.pack_into('<IIQQQQQQ', elf, 0x40, 1, 5, 0x100, 0, 0, 0, 0, 0x1000)
    size = len(elf) - 0x100
    struct.pack_into('<QQ', elf, 64 + 32, size, size)
    return bytes(elf)


def execute_projection(code, camera):
    """Interpret the copies, scalar multiplies and skip jump in this projection block.

    Decode the actual operands to check both the original AVX copy and its SSE
    replacement. This catches changes to the component or buffer offset written.
    """
    result = bytearray(0xd0)
    vector = bytes(16)
    pc = 0
    while pc < len(code):
        if code[pc:pc + 3] == bytes.fromhex('c6 01 01'):
            result[0] = 1
            pc += 3
        elif code[pc:pc + 4] == bytes.fromhex('f3 0f 59 05'):
            target = pc + 8 + struct.unpack_from('<i', code, pc + 4)[0]
            factor = struct.unpack_from('<f', code, target)[0]
            vector = struct.pack('<f', struct.unpack_from('<f', vector)[0] * factor) + vector[4:]
            pc += 8
        elif code[pc] == 0xe9:
            pc += 5 + struct.unpack_from('<i', code, pc + 1)[0]
            if pc != len(code):
                raise AssertionError('Projection jump must skip exactly its inline data')
        else:
            if code[pc:pc + 2] == bytes.fromhex('c5 f8'):
                op, operand = code[pc + 2], pc + 3
            elif code[pc] == 0x0f:
                op, operand = code[pc + 1], pc + 2
            else:
                raise AssertionError(f'Unexpected projection opcode at {pc:#x}')
            mode = code[operand]
            if (op, mode) in ((0x10, 0x43), (0x11, 0x41)):
                offset = struct.unpack_from('<b', code, operand + 1)[0]
                pc = operand + 2
            elif (op, mode) in ((0x10, 0x83), (0x11, 0x81)):
                offset = struct.unpack_from('<i', code, operand + 1)[0]
                pc = operand + 5
            else:
                raise AssertionError('Unexpected projection memory operand')
            if op == 0x10:
                vector = camera[offset:offset + 16]
                if len(vector) != 16:
                    raise AssertionError('Projection read outside camera')
            else:
                if not 0 <= offset <= len(result) - 16:
                    raise AssertionError('Projection write outside constant buffer')
                result[offset:offset + 16] = vector
    return result


class Aspect16x10Tests(unittest.TestCase):
    def test_default_and_legacy_widescreen_settings(self):
        for settings in ({}, {'aspect_ratio': '16x9'}, {'aspect_ratio': 'invalid'},
                         {'widescreen_layout': '16x10'},
                         {'aspect_ratio': '16x9', 'widescreen_layout': '21x9'}):
            self.assertIsNone(aspect_ratio.selection(settings))
        for ratio in ('21x9', '32x9'):
            legacy = {'widescreen_layout': ratio, 'render_res': '1440'}
            modern = dict(legacy, aspect_ratio=ratio, aspect_output=widescreen.OUTPUTS[ratio][-1])
            self.assertEqual(aspect_ratio.selection(legacy), widescreen.selection(legacy))
            self.assertEqual(aspect_ratio.selection(modern), widescreen.selection(modern))
            self.assertEqual(widescreen.selection(modern), (ratio, modern['aspect_output'], '1440'))
        self.assertIsNone(widescreen.selection({'aspect_ratio': '16x10', 'widescreen_layout': '21x9'}))

    def test_only_wide_ratios_delegate_to_corrected_layouts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ini = root / 'bbport.ini'
            for ratio in ('21x9', '32x9'):
                ini.write_text(f'aspect_ratio={ratio}\n')
                with patch.object(widescreen, 'prepare', return_value={'wide': ratio}) as prepare:
                    self.assertEqual(aspect_ratio.prepare(root, root, ini, {}, 'CUSA13795', '01.21', {}),
                                     {'wide': ratio})
                    prepare.assert_called_once()
            ini.write_text('aspect_ratio=16x9\nwidescreen_layout=21x9\n')
            with patch.object(widescreen, 'prepare') as prepare:
                self.assertIsNone(aspect_ratio.prepare(root, root, ini, {}, 'CUSA13795', '01.21', {}))
                prepare.assert_not_called()

    def test_shell_launch_plan_keeps_messages_off_stdout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'eboot.elf').write_bytes(game_elf(game_code()))
            ini = root / 'bbport.ini'
            ini.write_text('aspect_ratio=16x10\naspect_output=1280x800\n')
            result = subprocess.run([sys.executable, str(ROOT / 'scripts/aspect_ratio.py'), str(root),
                                     '--out', str(root), '--settings', str(ini)],
                                    capture_output=True, text=True, check=True)
            plan = dict(line.split('=', 1) for line in result.stdout.splitlines())
            self.assertEqual(Path(plan['settings']), root / 'aspect_ratio/bbport.ini')
            self.assertEqual(Path(plan['patches_dir']), root / 'aspect_ratio/patches')
            self.assertEqual(plan['env.BB_HUD_SCALE'], '')
            self.assertEqual(plan['archive_patches'], '')
            self.assertIn('Aspect ratio: 16:10 at 1280x800', result.stderr)

    def test_selection(self):
        for output in aspect_ratio.OUTPUTS['16x10']:
            self.assertEqual(aspect_ratio.selection({'aspect_ratio': '16x10',
                                                  'aspect_output': output, 'render_res': '720'}),
                             ('16x10', output, '720'))
        self.assertEqual(aspect_ratio.selection({'aspect_ratio': '16x10',
                                              'aspect_output': '3440x1440', 'render_res': '900'}),
                         ('16x10', '1920x1200', '1080'))

    def test_every_size_internal_resolution_and_edition_uses_checked_private_settings(self):
        for title in ('CUSA13795', 'CUSA14876'):
            for output in aspect_ratio.OUTPUTS['16x10']:
                for internal in aspect_ratio.INTERNAL:
                    with self.subTest(title=title, output=output, internal=internal), tempfile.TemporaryDirectory() as tmp:
                        root = Path(tmp)
                        out = root / 'out'
                        out.mkdir()
                        elf = game_elf(game_code())
                        (out / 'eboot.elf').write_bytes(elf)
                        ini = root / 'bbport.ini'
                        original = (f'aspect_ratio=16x10\naspect_output={output}\nrender_res={internal}\n'
                                    'temporal=dlss\nupscaler=fsr3\nonline=0\nultrawide=1\nshow_fps=1\n')
                        ini.write_text(original)
                        env = {'BB_HUD_SCALE': 'old shader override'}
                        base = {'/app0/archives/update2.pak': root / 'playlist.bbpatch'}
                        with patch.object(widescreen, 'archive_patches') as assets:
                            plan = aspect_ratio.prepare(root / 'game', out, ini, env, title, '01.21', base)
                        assets.assert_not_called()
                        self.assertIsNotNone(plan)
                        self.assertEqual(ini.read_text(), original)
                        self.assertEqual(env, {'BB_HUD_SCALE': 'old shader override'})
                        self.assertEqual(plan['archive_patches'], {})
                        self.assertIn('playlist.bbpatch', aspect_ratio.archive_list(base, plan))
                        self.assertEqual(plan['env'], {'BB_TEMPORAL': 'off', 'BB_HUD_SCALE': ''})
                        settings = patches.read_settings(plan['config'])
                        self.assertEqual(settings['layout_internal'], internal)
                        for key, value in {'render_res': '1080', 'temporal': 'off', 'upscaler': 'off',
                                           'ultrawide': '0', 'patch_pro_1080p': '0', 'show_fps': '1'}.items():
                            self.assertEqual(settings[key], value)
                        xml = ET.parse(plan['patches_dir'] / 'AspectRatio.xml').getroot()
                        self.assertEqual(xml.find('./TitleID/ID').text, title)
                        lines = list(xml.iter('Line'))
                        self.assertEqual(len(lines), 4)
                        values = [patches.encode(line) for line in lines if len(patches.encode(line)) == 5]
                        width, height = map(int, output.split('x'))
                        self.assertEqual(values, [b'\xb8' + struct.pack('<I', width),
                                                  b'\xb8' + struct.pack('<I', height)])
                        selected = [('layout', None, xml.find('Metadata'))] + patches.render_patches(settings)
                        writes = patches.compile_external(selected, patches.eboot_segments(elf), elf,
                                                          strict=True, require_verification=True)
                        spans = sorted((offset, offset + len(data)) for offset, data in writes)
                        self.assertTrue(all(end <= other for (_, end), (other, _) in zip(spans, spans[1:])))

    def test_missing_or_ambiguous_code_leaves_no_layout_or_settings(self):
        signatures = [bytes.fromhex(patches.OUTPUT_SIZE_SIGNATURE)] + [
            bytes.fromhex(line.get('Signature'))
            for line in ET.parse(aspect_ratio.DATA / '16x10.xml').iter('Line')]
        original = game_code()
        cases = [code for signature in signatures for code in (
            original.replace(signature, bytes(len(signature))), original + signature)]
        for code in cases:
            with self.subTest(code_bytes=len(code)), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / 'eboot.elf').write_bytes(game_elf(code))
                ini = root / 'bbport.ini'
                ini.write_text('aspect_ratio=16x10\n')
                self.assertIsNone(aspect_ratio.prepare(root / 'game', root, ini, {}, 'CUSA13795', '01.21', {}))
                self.assertEqual(ini.read_text(), 'aspect_ratio=16x10\n')
                self.assertFalse((root / 'aspect_ratio').exists())

    def test_unsupported_game_is_unchanged(self):
        for title, version in (('CUSA12345', '01.21'), ('CUSA14876', '01.00')):
            with self.subTest(title=title, version=version), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                ini = root / 'bbport.ini'
                ini.write_text('aspect_ratio=16x10\n')
                self.assertIsNone(aspect_ratio.prepare(root / 'game', root / 'out', ini, {}, title, version, {}))
                self.assertFalse((root / 'out').exists())

    def test_missing_or_invalid_projection_data_falls_back_without_writing(self):
        for data in (None, '<Patch>', '<Patch />',
                     '<Patch><TitleID><ID>CUSA13795</ID></TitleID>'
                     '<Metadata AppVer="01.00" AppElf="eboot.bin"><PatchList><Line /></PatchList></Metadata></Patch>'):
            with self.subTest(data=data), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                if data is not None:
                    (root / '16x10.xml').write_text(data)
                ini = root / 'bbport.ini'
                ini.write_text('aspect_ratio=16x10\n')
                with patch.object(aspect_ratio, 'DATA', root):
                    self.assertIsNone(aspect_ratio.prepare(root / 'game', root / 'out', ini, {},
                                                        'CUSA13795', '01.21', {}))
                self.assertFalse((root / 'out').exists())

    def test_projection_changes_only_x_and_preserves_vertical_fov(self):
        line = projection_line()
        original, adapted = bytes.fromhex(line.get('Signature')), bytes.fromhex(line.get('Value'))
        for degrees in (45, 60, 90, 110):
            with self.subTest(vertical_fov=degrees):
                camera = bytearray(0xf0)
                for offset in range(0x70, 0xf0, 16):
                    struct.pack_into('<4f', camera, offset, offset / 16, .25, .5, .75)
                vertical = 1 / math.tan(math.radians(degrees) / 2)
                struct.pack_into('<f', camera, 0xb0, vertical / (16 / 9))
                struct.pack_into('<f', camera, 0xc4, vertical)
                before, after = execute_projection(original, camera), execute_projection(adapted, camera)
                expected = bytearray(before)
                factor = struct.unpack('<f', struct.pack('<f', (16 / 9) / (16 / 10)))[0]
                for row in range(4):
                    at = 0x90 + row * 16
                    struct.pack_into('<f', expected, at, struct.unpack_from('<f', before, at)[0] * factor)
                self.assertEqual(after, expected)
                x = struct.unpack_from('<f', after, 0x90)[0]
                y = struct.unpack_from('<f', after, 0xa4)[0]
                self.assertAlmostEqual(y, vertical, places=6)
                for output in aspect_ratio.OUTPUTS['16x10']:
                    width, height = map(int, output.split('x'))
                    self.assertAlmostEqual(x * width / (y * height), 1, places=6)


if __name__ == '__main__':
    unittest.main()
