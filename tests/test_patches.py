from paths import ROOT
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import patches
from patches import (EBOOT_BASE, GAME_PATCH_KEYS, compile_external, eboot_segments, encode,
                     external_patches, external_selection, read_settings, write_blob)

SEGMENTS = [(0, 0x6000000)]

EXTERNAL = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA13795</ID></TitleID>
  <Metadata Title="CTR" Name="On" Author="x" PatchVer="1.0" AppVer="01.21" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00500000" Value="9090"/></PatchList>
  </Metadata>
  <Metadata Title="CTR" Name="Off" Author="x" PatchVer="1.0" AppVer="01.21" AppElf="eboot.bin">
    <PatchList><Line Type="bytes32" Address="0x00500010" Value="0x11223344"/></PatchList>
  </Metadata>
  <Metadata Title="CTR" Name="Mask" Author="x" PatchVer="1.0" AppVer="01.21" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="mask" Address="" Value="90 90"/></PatchList>
  </Metadata>
  <Metadata Title="CTR" Name="Old" Author="x" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00500020" Value="90"/></PatchList>
  </Metadata>
</Patch>
"""


class ExternalPatchTests(unittest.TestCase):
    def test_selection_filters_version_title_and_mask_patches(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / 'extra.xml').write_text(EXTERNAL)
            (folder / 'other.xml').write_text(EXTERNAL.replace('CUSA13795', 'CUSA99999'))
            found = external_patches(folder)
            self.assertEqual([key for key, _, _ in found], ['extra.xml/On', 'extra.xml/Off', 'extra.xml/Mask'])
            # isEnabled selects On and Mask; Mask is skipped whole for its unsupported line.
            writes = compile_external(external_selection(found, None), SEGMENTS)
            self.assertEqual(writes, [(0x100000, b'\x90\x90')])
            config = folder / 'patches.json'
            config.write_text(json.dumps({'enabled': ['extra.xml/Off'], 'disabled': ['extra.xml/On']}))
            writes = compile_external(external_selection(found, config), SEGMENTS)
            self.assertEqual(writes, [(0x100010, b'\x44\x33\x22\x11')])

    def test_out_of_segment_patch_is_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / 'extra.xml').write_text(EXTERNAL)
            found = external_patches(folder)
            self.assertEqual(compile_external(external_selection(found, None), [(0, 0x1000)]), [])

    def test_encode_types(self):
        import xml.etree.ElementTree as ET
        line = ET.fromstring('<Line Type="float32" Address="0x400000" Value="1.0"/>')
        self.assertEqual(encode(line), struct.pack('<f', 1.0))
        line = ET.fromstring('<Line Type="utf8" Address="0x400000" Value="ab"/>')
        self.assertEqual(encode(line), b'ab\0')
        with self.assertRaises(ValueError):
            encode(ET.fromstring('<Line Type="mask" Address="" Value="90"/>'))

    def test_eboot_segments_and_blob(self):
        elf = bytearray(120)
        struct.pack_into('<Q', elf, 0x20, 64)
        struct.pack_into('<HH', elf, 0x36, 56, 1)
        struct.pack_into('<IIQQQQQQ', elf, 64, 1, 0, 0, 0x1000, 0, 0, 0x2000, 0)
        self.assertEqual(eboot_segments(bytes(elf)), [(0x1000, 0x3000)])
        with tempfile.TemporaryDirectory() as tmp:
            write_blob(Path(tmp), [(0x10, b'\x90')])
            blob = (Path(tmp) / 'patches.bin').read_bytes()
            self.assertEqual(blob[:24], struct.pack('<8sQQ', b'BBPATCH2', EBOOT_BASE, 1))
            self.assertEqual(blob[24:], struct.pack('<QQ', 0x10, 1) + b'\x90')


def tiny_elf(code, vaddr=0x1000, offset=0x100):
    """An ELF64 header with one load segment holding code at vaddr (file offset offset)."""
    elf = bytearray(offset) + code
    struct.pack_into('<Q', elf, 0x20, 64)
    struct.pack_into('<HH', elf, 0x36, 56, 1)
    struct.pack_into('<IIQQQQQQ', elf, 64, 1, 5, offset, vaddr, vaddr, len(code), len(code), 0x10)
    return bytes(elf)


class SignatureTests(unittest.TestCase):
    SIGNED = """<?xml version="1.0"?><Patch><TitleID><ID>CUSA13795</ID></TitleID>
      <Metadata Title="CTR" Name="S" Author="x" AppVer="01.21" AppElf="eboot.bin" isEnabled="true">
        <PatchList><Line Type="bytes" Address="{address}" Value="9090" Signature="aa bb ?? dd" SignatureOffset="0x2"/></PatchList>
      </Metadata></Patch>"""

    def writes(self, elf, address):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'p.xml').write_text(self.SIGNED.format(address=hex(address)))
            found = external_patches(Path(tmp))
            return compile_external(external_selection(found, None), eboot_segments(elf), elf)

    def test_signature_at_the_address(self):
        elf = tiny_elf(b'\0' * 16 + b'\xaa\xbb\x01\xdd' + b'\0' * 12)
        self.assertEqual(self.writes(elf, EBOOT_BASE + 0x1012), [(0x1012, b'\x90\x90')])

    def test_signature_moved(self):
        # The 6.72+ build: the same code at another address.
        elf = tiny_elf(b'\0' * 4 + b'\xaa\xbb\x07\xdd' + b'\0' * 24)
        self.assertEqual(self.writes(elf, EBOOT_BASE + 0x1012), [(0x1006, b'\x90\x90')])

    def test_signature_missing_or_ambiguous_skips_the_patch(self):
        self.assertEqual(self.writes(tiny_elf(b'\0' * 32), EBOOT_BASE + 0x1012), [])
        twice = tiny_elf(b'\xaa\xbb\x00\xdd' * 2 + b'\0' * 24)
        self.assertEqual(self.writes(twice, EBOOT_BASE + 0x1012), [])


class RenderResTests(unittest.TestCase):
    """render_res: the game's PS4 Pro render scale and GPU heap (docs/sacredfire/13-resolution.md)."""

    def elf(self):
        scale = bytes.fromhex(patches.RENDER_SCALE_SIGNATURE.replace(' ', ''))
        heap = bytes.fromhex(patches.GPU_HEAP_SIGNATURE.replace(' ', ''))
        # Both sites at their 5.05-build addresses inside one segment from 0x13a9000.
        base = 0x13a9000
        code = bytearray(0x1fbb940 - EBOOT_BASE - base + 16)
        at = 0x17aa662 - EBOOT_BASE - base
        code[at:at + len(scale)] = scale
        at = 0x1fbb938 - EBOOT_BASE - base
        code[at:at + len(heap)] = heap
        return tiny_elf(bytes(code), vaddr=base)

    def test_selection_follows_render_res(self):
        found = external_patches(ROOT / 'patches')
        pro = GAME_PATCH_KEYS['patch_pro_1080p']
        keys = lambda ini: [k for k, _, _ in external_selection(found, None, ini)]
        self.assertNotIn(pro, keys({'render_res': '1080', 'patch_pro_1080p': '1'}))
        self.assertNotIn(pro, keys({'render_res': '2160', 'patch_pro_1080p': '1'}))
        self.assertIn(pro, keys({'patch_pro_1080p': '1'}))  # older settings without render_res

    def test_builtin_writes(self):
        elf = self.elf()
        segments = eboot_segments(elf)
        self.assertEqual(patches.render_patches({'render_res': '1080'}), [])
        self.assertEqual(patches.render_patches({'render_res': '1440'}), [])
        writes = compile_external(patches.render_patches({'render_res': '1620'}), segments, elf)
        self.assertEqual(writes, [(0x17aa662 - EBOOT_BASE, b'\xb8' + struct.pack('<f', 1.5))])
        writes = compile_external(patches.render_patches({'render_res': '2160'}), segments, elf)
        self.assertEqual(writes, [(0x17aa662 - EBOOT_BASE, b'\xb8' + struct.pack('<f', 2.0)),
                                  (0x1fbb93e - EBOOT_BASE, b'\x70')])

    def test_ultrawide_plan_and_writes(self):
        plan = patches.render_plan
        uw = {'ultrawide': '1', 'ultrawide_size': '3440x1440', 'render_res': '2160'}
        self.assertEqual(plan(uw)['output'], (3440, 1440))
        self.assertFalse(plan(uw)['pro'])  # the base path: the monitor's size, not render_res
        self.assertEqual(plan({**uw, 'ultrawide': '0'})['label'], '2160p')
        self.assertEqual(plan({'ultrawide': '1'}), None)  # no size chosen yet
        self.assertEqual(plan({'ultrawide': '1', 'ultrawide_size': '99x99'}), None)
        sig = bytes.fromhex(patches.OUTPUT_SIZE_SIGNATURE.replace(' ', ''))
        elf = tiny_elf(bytes(0x100) + sig + bytes(0x100), vaddr=0x1000)
        writes = compile_external(patches.render_patches(uw), eboot_segments(elf), elf)
        self.assertEqual(writes, [(0x1100, b'\xb8' + struct.pack('<I', 3440)),
                                  (0x1115, b'\xb8' + struct.pack('<I', 1440))])

    def test_dlss_quality_picks_the_render_size(self):
        plan = patches.render_plan
        dlss = lambda q, out='1080': {'temporal': 'dlss', 'dlss_quality': q, 'dlss_output': out,
                                       'render_res': '1440'}
        self.assertEqual(plan(dlss('dlaa'))['pro'], False)          # native 1080p, base path
        p = plan(dlss('performance'))                               # 540p to 1080p
        self.assertAlmostEqual(p['scale'], 0.5)
        self.assertTrue(p['pro'] and p['single'] and not p['heap'])
        p = plan(dlss('quality', '2160'))                           # 1440p to 4K
        self.assertAlmostEqual(p['scale'], 4 / 3)
        self.assertTrue(p['pro'] and not p['single'] and not p['heap'])
        self.assertTrue(plan(dlss('dlaa', '2160'))['heap'])         # native 4K
        # Automatic (or another upscaler): render_res decides.
        self.assertEqual(plan({**dlss('auto'), 'render_res': '720'})['label'], '720p')
        self.assertEqual(plan({**dlss('quality'), 'temporal': 'fsr3'})['label'], '1440p')


class ThemeTests(unittest.TestCase):
    """Season events: the hidden-object mode read from the online config replaced by a constant."""

    def elf(self):
        code = bytes.fromhex(patches.THEME_SIGNATURE.replace(' ', '')) + b'\0' * 4
        return tiny_elf(b'\0' * 0x1b + code + b'\0' * 16, vaddr=0xfb6000)

    def test_normal_season_writes_nothing(self):
        with patch.dict('os.environ', {'BB_THEME': ''}):
            self.assertEqual(patches.theme_patches({}), [])
            self.assertEqual(patches.theme_patches({'theme': 'default'}), [])
            self.assertEqual(patches.theme_patches({'theme': 'pumpkin'}), [])

    def test_mode_from_server_or_ini(self):
        elf = self.elf()
        segments = eboot_segments(elf)
        want = lambda mode: [(0xfb601b, bytes.fromhex(f'4c89efbe{mode:02x}0000000f1f440000'))]
        with patch.dict('os.environ', {'BB_THEME': ''}):
            self.assertEqual(compile_external(patches.theme_patches({'theme': 'golden'}), segments, elf), want(2))
        # Online, the server's season wins over bbport.ini.
        with patch.dict('os.environ', {'BB_THEME': 'halloween'}):
            self.assertEqual(compile_external(patches.theme_patches({'theme': 'golden'}), segments, elf), want(1))
        with patch.dict('os.environ', {'BB_THEME': 'default'}):
            self.assertEqual(patches.theme_patches({'theme': 'golden'}), [])

    def test_replacement_keeps_the_call(self):
        # 13 bytes replaced, so the call to setMode (e8, the signature's last byte) stays in place.
        self.assertEqual(len(bytes.fromhex('4c89efbe010000000f1f440000')), 13)
        self.assertEqual(len(bytes.fromhex(patches.THEME_SIGNATURE.replace(' ', ''))), 14)


class GamePatchTests(unittest.TestCase):
    """The bundled CTR patches, chosen through bbport.ini patch_* keys (the in-game menu)."""

    def test_bundled_file_lists_every_menu_patch(self):
        found = external_patches(ROOT / 'patches')
        keys = {key for key, _, _ in found}
        for patch in GAME_PATCH_KEYS.values():
            self.assertIn(patch, keys)
        self.assertEqual(external_patches(ROOT / 'patches', title_id='CUSA99999'), [])
        self.assertEqual(external_patches(ROOT / 'patches', app_version='01.00'), [])

    def test_eu_edition_patches_follow_menu_keys(self):
        # The three base-game patches (60 FPS, motion blur, 1080p Pro); the online key and
        # Camiloop's unlock patch also have EU files for the 1.21 update.
        classic = [k for k in GAME_PATCH_KEYS if k not in ('patch_unlock_all', 'patch_online_key')]
        found = external_patches(ROOT / 'patches', app_version='01.00', title_id='CUSA14876')
        self.assertEqual({key.split('/')[0] for key, _, _ in found}, {'CrashTeamRacingEU.xml'})
        self.assertEqual(len(found), len(classic))
        found_121 = external_patches(ROOT / 'patches', app_version='01.21', title_id='CUSA14876')
        self.assertEqual(len(found_121), len(classic) + 2)  # EU 1.21 also has online and unlock patches
        self.assertIn('CrashTeamRacingOnlineEU.xml/Online: custom Demonware key', [k for k, _, _ in found_121])
        self.assertEqual(external_patches(ROOT / 'patches', app_version='01.10', title_id='CUSA14876'), [])
        selected = external_selection(found, None, {'patch_60fps': '1'})
        self.assertEqual([key for key, _, _ in selected], ['CrashTeamRacingEU.xml/60 FPS Unlock'])

    def test_ini_keys_select_patches(self):
        found = external_patches(ROOT / 'patches')
        self.assertEqual(external_selection(found, None, {}), [])
        selected = external_selection(found, None, {'patch_60fps': '1', 'patch_no_motion_blur': '0'})
        self.assertEqual([key for key, _, _ in selected], ['CrashTeamRacing.xml/60 FPS Unlock'])
        signature = selected[0][2].find('.//Line').get('Signature')
        code = bytes(0 if b == '??' else int(b, 16) for b in signature.split())
        start = 0x017a9d8b - EBOOT_BASE  # the 5.05 build's address for the signature
        for at in (start, start + 0x40):  # 5.05 build; code moved (6.72+ build)
            elf = tiny_elf(b'\0' * (at - 0x13a9d00) + code + b'\0' * 32, vaddr=0x13a9d00)
            writes = compile_external(selected, eboot_segments(elf), elf)
            self.assertEqual(writes, [(at + 5, bytes.fromhex('41899688040000'))])

    def test_online_environment_overrides_ini_patch(self):
        found = external_patches(ROOT / 'patches')
        online = 'CrashTeamRacingOnline.xml/Online: custom Demonware key'
        keys = lambda ini: [key for key, _, _ in external_selection(found, None, ini)]
        with patch.dict('os.environ', {'BB_ONLINE': '0'}):
            self.assertNotIn(online, keys({'online': '1'}))
        with patch.dict('os.environ', {'BB_ONLINE': '1'}):
            self.assertIn(online, keys({'online': '0'}))

    def test_read_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            ini = Path(tmp) / 'bbport.ini'
            ini.write_text('# comment\npatch_60fps=1\nfps_counter = 0\n')
            self.assertEqual(read_settings(ini), {'patch_60fps': '1', 'fps_counter': '0'})
            self.assertEqual(read_settings(Path(tmp) / 'missing.ini'), {})


if __name__ == '__main__':
    unittest.main()
