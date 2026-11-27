"""The Discord presence's track pictures list (scripts/track_names.py presence_images)."""
import json
import tempfile
import unittest
from pathlib import Path

from paths import ROOT  # noqa: F401  (scripts/ on the path)
import track_names

BASE = 'https://pictures.example/presence/'


class PresenceImagesTests(unittest.TestCase):
    def test_index_to_lines_and_kept_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            index = {'format': 1, 'zones': ['t111_crash_cove', 't929_neo_bowser_city', '../x', 'BAD ZONE', 7]}
            target = track_names.presence_images(tmp, base=BASE, fetch=lambda url: json.dumps(index).encode())
            self.assertEqual(target.read_text(encoding='utf-8').splitlines(),
                             [f't111_crash_cove\t{BASE}t111_crash_cove.jpg',
                              f't929_neo_bowser_city\t{BASE}t929_neo_bowser_city.jpg'])

            def offline(url):
                raise OSError('no answer')
            # the bucket does not answer: the list it gave last time
            self.assertEqual(track_names.presence_images(tmp, base=BASE, fetch=offline).read_text(encoding='utf-8'),
                             target.read_text(encoding='utf-8'))
            # another bucket: not the kept copy of this one
            self.assertIsNone(track_names.presence_images(tmp, base='https://other.example/p/', fetch=offline))
            self.assertFalse((Path(tmp) / 'presence_images.txt').exists())

    def test_off_and_bad_base(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'presence_images.txt').write_text('old\n', encoding='utf-8')
            self.assertIsNone(track_names.presence_images(tmp, base='', fetch=lambda url: b'{}'))
            self.assertFalse((Path(tmp) / 'presence_images.txt').exists())
            self.assertIsNone(track_names.presence_images(tmp, base='http://plain.example/', fetch=lambda url: b'{}'))
            self.assertIsNone(track_names.presence_images(tmp, base=BASE, fetch=lambda url: b'{"format": 2}'))


if __name__ == '__main__':
    unittest.main()
