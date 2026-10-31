"""HUD overlays compose with other launch features and reject incomplete asset sets."""
from paths import ROOT
import io
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import archive_patch
import aspect_hud


def archive_file(path):
    data = bytearray(0x38 + 20 * 3)
    struct.pack_into('<II', data, 0, 0x1a414749, 13)
    struct.pack_into('<I', data, 12, 3)
    for index in range(3):
        payload = b'original ' + bytes([index])
        offset = len(data)
        data.extend(payload)
        struct.pack_into('<4I', data, 0x38 + 4 * 3 + 16 * index,
                         offset, 0, len(payload), 0xffffffff)
    path.write_bytes(data)


def entry(path, overlay, index):
    record = archive_patch.view(path, overlay, 0x38 + 4 * 3 + 16 * index, 16)
    offset, extra, length, mode = struct.unpack('<4I', record)
    assert mode == 0xffffffff
    return archive_patch.view(path, overlay, offset | (extra & 0xff) << 32, length)


class Archive:
    """Only the catalog/extraction are synthetic; overlay reads/writes use the real format."""
    def __init__(self, path):
        self._file = Path(path).open('rb')
        self.data = io.BytesIO(self._file.read())
        self.files = [{'index': 0, 'path': 'ui/Octane_Adventure_HUD.igz'},
                      {'index': 1, 'path': 'ui/GuiSharedAssets/Octane_NFBoostBar.igz'}]

    def extract(self, entry):
        return b'HUD' if entry['index'] == 0 else b'gauge'


class HudPlanTests(unittest.TestCase):
    def test_composition_and_cache_keep_the_previous_overlay(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'game/archives/update.pak'
            source.parent.mkdir(parents=True)
            archive_file(source)
            original = source.read_bytes()
            prior = root / 'prior.bbpatch'
            guest = '/app0/archives/update.pak'
            archive_patch.write_entries_patch(prior, source, guest, {2: b'previous overlay'})
            with patch.object(aspect_hud, 'Archive', Archive), \
                 patch.object(aspect_hud, 'correct_boost_bar', return_value=b'corrected gauge') as gauge:
                made = aspect_hud.prepare(root / 'game', root / 'out', {guest: prior})
                self.assertEqual(entry(source, made[guest], 0), b'original \x00')
                self.assertEqual(entry(source, made[guest], 1), b'corrected gauge')
                self.assertEqual(entry(source, made[guest], 2), b'previous overlay')
                self.assertEqual(source.read_bytes(), original)
                gauge.assert_called_once_with(b'gauge')
                gauge.reset_mock()
                self.assertEqual(aspect_hud.prepare(root / 'game', root / 'out', {guest: prior}), made)
                gauge.assert_not_called()
                made[guest].unlink()
                self.assertEqual(aspect_hud.prepare(root / 'game', root / 'out', {guest: prior}), made)
                self.assertEqual(entry(source, made[guest], 2), b'previous overlay')
                gauge.assert_called_once()

    def test_a_mod_replacing_the_same_asset_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'game/archives/update.pak'
            source.parent.mkdir(parents=True)
            archive_file(source)
            prior = root / 'prior.bbpatch'
            guest = '/app0/archives/update.pak'
            archive_patch.write_entries_patch(prior, source, guest, {1: b'custom gauge'})
            with patch.object(aspect_hud, 'Archive', Archive), \
                 patch.object(aspect_hud, 'correct_boost_bar', return_value=b'corrected gauge'):
                with self.assertRaisesRegex(ValueError, 'already replaced'):
                    aspect_hud.prepare(root / 'game', root / 'out', {guest: prior})
            self.assertEqual(entry(source, prior, 1), b'custom gauge')
            self.assertFalse(list((root / 'out').rglob('stamp.json')))

    def test_incomplete_assets_are_not_published(self):
        class Incomplete(Archive):
            def __init__(self, path):
                super().__init__(path)
                self.files = self.files[:1]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'game/archives/update.pak'
            source.parent.mkdir(parents=True)
            archive_file(source)
            with patch.object(aspect_hud, 'Archive', Incomplete):
                with self.assertRaisesRegex(ValueError, 'assets are missing'):
                    aspect_hud.prepare(root / 'game', root / 'out', {})
            self.assertFalse(list((root / 'out').rglob('*.bbpatch')))

    def test_executable_only_preparation_needs_no_archives(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertEqual(aspect_hud.prepare(root / 'game', root / 'out', {}), {})
            self.assertFalse((root / 'out').exists())


if __name__ == '__main__':
    unittest.main()
