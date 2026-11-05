"""Startup optimizations must preserve archive bytes and IGZ reference order."""
import struct
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
import sys
sys.path.insert(0, str(ROOT / 'scripts'))
from archive_reader import Archive
from igz_file import Igz


def archive_blob(payload):
    names = struct.pack('<I', 4) + b'asset\0asset\0'
    blob = bytearray(128)
    struct.pack_into('<5I', blob, 0, 0x1a414749, 13, 0, 1, 2048)
    struct.pack_into('<QI', blob, 40, 76, len(names))
    struct.pack_into('<4I', blob, 60, 128, 0, len(payload), 0xffffffff)
    blob[76:76 + len(names)] = names
    return bytes(blob) + payload


class StartupPreparationTests(unittest.TestCase):
    def test_nested_mapping_preserves_relative_offsets_and_releases_view(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'parent.pak'
            path.write_bytes(archive_blob(archive_blob(b'unchanged payload')))
            outer = Archive(path)
            try:
                with outer.nested(outer.files[0]) as child:
                    self.assertEqual(child.files[0]['offset'], 128)
                    self.assertEqual(child.extract(child.files[0]), b'unchanged payload')
                    self.assertIs(child.data.obj, outer.data)
                self.assertEqual(list(Path(directory).iterdir()), [path])
                with self.assertRaisesRegex(RuntimeError, 'interrupted'):
                    with outer.nested(outer.files[0]):
                        raise RuntimeError('interrupted')
            finally:
                outer.data.close()  # Raises BufferError if either child leaked a view.
                outer._file.close()

    def test_nested_mapping_rejects_out_of_bounds_and_compression(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'parent.pak'
            path.write_bytes(archive_blob(archive_blob(b'payload')))
            outer = Archive(path)
            try:
                for changed in ({'size': len(outer.data)}, {'compression': 0}, {'offset': -1}):
                    with self.assertRaises(ValueError):
                        with outer.nested(dict(outer.files[0], **changed)):
                            self.fail('invalid child accepted')
            finally:
                outer.data.close()
                outer._file.close()

    def test_add_site_keeps_unique_sorted_offsets(self):
        graph = object.__new__(Igz)
        graph.sites = {'RSTT': [8, 16, 32]}
        for offset in (24, 0, 32, 40, 8):
            graph.add_site('RSTT', offset)
        self.assertEqual(graph.sites['RSTT'], [0, 8, 16, 24, 32, 40])


if __name__ == '__main__':
    unittest.main()
