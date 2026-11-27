"""The port's new tracks (scripts/track_content.py): the pieces that need no game files."""
from paths import ROOT  # noqa: F401
import json
import shutil
import tempfile
import threading
import unittest
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import track_content as tc


def package(root, **changes):
    folder = Path(root) / 'tracks' / 'neo'
    (folder / 'archives').mkdir(parents=True)
    for name in ('t929_neo_bowser_city.pak', 'update3.pak'):
        (folder / 'archives' / name).write_bytes(b'x')
    m = {'id': 'neo_bowser_city', 'level': 'octane/t929_neo_bowser_city/t929_neo_bowser_city',
         'archives': ['archives/t929_neo_bowser_city.pak'], 'registration': 'archives/update3.pak',
         'files': [], 'name_source': 't114_sewer_speedway', 'names': {'en': 'Neo Bowser City'}}
    m.update(changes)
    (folder / 'track.json').write_text(json.dumps(m), encoding='utf-8')
    return folder


class NameTests(unittest.TestCase):
    def test_title_and_text_path(self):
        self.assertEqual(tc.title('t929_neo_bowser_city'), 'T929_Neo_Bowser_City')
        self.assertEqual(tc.lng_path('t929_neo_bowser_city', 'mx',
                                     'maps/Octane/T114_Sewer_Speedway/T114_Sewer_Speedway_zoneInfo_mx.lng'),
                         'maps/Octane/T929_Neo_Bowser_City/T929_Neo_Bowser_City_zoneInfo_mx.lng')

    def test_only_zone_texts_are_renamed(self):
        rename = tc.renamer('T114_Sewer_Speedway', 'T929_Neo_Bowser_City')
        self.assertEqual(rename('T114_Sewer_Speedway_zoneInfo_en'), 'T929_Neo_Bowser_City_zoneInfo_en')
        self.assertEqual(rename('<build>project:/temporary/octane/data/ps4/output/maps/Octane/T114_Sewer_Speedway/'
                                'T114_Sewer_Speedway_zoneInfo_de.lng'),
                         '<build>project:/temporary/octane/data/ps4/output/maps/Octane/T929_Neo_Bowser_City/'
                         'T929_Neo_Bowser_City_zoneInfo_de.lng')
        # the source track's other pieces it still uses (minimap, icon) keep their names
        self.assertEqual(rename('T114_Sewer_Speedway_Minimap_materials'), 'T114_Sewer_Speedway_Minimap_materials')


class BackgroundTests(unittest.TestCase):
    def test_a_flat_block_is_one_colour(self):
        import season_art
        block = tc.bc1_block([bytes([128, 64, 32])] * 16)
        self.assertEqual(set(season_art.decode_colours(block)), {season_art.rgb565(season_art.to565((128, 64, 32)))})

    def test_opaque_bc3_rows_from_the_bottom_up(self):
        import season_art
        width, height = 8, 8
        rgba = b''.join(bytes([255, 255, 255, 255] if y < 4 else [0, 0, 0, 255]) for y in range(height) for _x in range(width))
        out = tc.bc3_picture(width, height, rgba)
        across, down = season_art.block_grid(width, height)
        self.assertEqual(len(out), across * down * 16)
        top = out[season_art.tiled_index(0, 0, across) * 16:][:16]
        bottom = out[season_art.tiled_index(0, 1, across) * 16:][:16]
        self.assertEqual(top[:2], bytes([255, 255]))     # opaque
        # the picture's top (white) is stored last: block row 1
        self.assertEqual(season_art.decode_colours(top[8:])[0], (0, 0, 0))
        self.assertEqual(season_art.decode_colours(bottom[8:])[0], (255, 255, 255))


class IconTests(unittest.TestCase):
    def test_a_white_icon_in_the_games_bc3_layout(self):
        import season_art
        w = h = 100
        rgba = bytearray(w * h * 4)
        for y in range(h):
            for x in range(w):
                rgba[(y * w + x) * 4:(y * w + x) * 4 + 4] = b'\xff\xff\xff' + bytes([255 if x < 50 else 0])
        blocks = tc.bc3_icon(w, h, bytes(rgba))
        across, down = season_art.block_grid(w, h)
        self.assertEqual(len(blocks), across * down * 16)          # 16384, as the game's 100x100 icons
        left = season_art.tiled_index(0, 0, across) * 16
        right = season_art.tiled_index(20, 0, across) * 16
        self.assertEqual(blocks[left:left + 2], b'\xff\xff')        # opaque block
        self.assertEqual(blocks[right:right + 2], b'\x00\x00')      # transparent block
        self.assertEqual(blocks[left + 8:left + 16], b'\xff\xff\xff\xff\x00\x00\x00\x00')   # white


class DownloadTests(unittest.TestCase):
    def settings(self, root, text):
        path = Path(root) / 'bbport.ini'
        path.write_text(text, encoding='utf-8')
        return path

    def test_where_the_tracks_are(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertEqual(tc.content_url(self.settings(root, 'online_server=129.151.105.177\n')),
                             'http://129.151.105.177:8094/content/')
            self.assertEqual(tc.content_url(self.settings(root, 'online_server=octr.ryu7w7.xyz\n')),
                             'https://octr.ryu7w7.xyz/content/')
            self.assertEqual(tc.content_url(self.settings(root, 'online_server=x\ncontent_url=http://h/c\n')),
                             'http://h/c/')

    def test_manifest_paths_stay_in_their_folder(self):
        for good in ('track.json', 'archives/t929_neo_bowser_city.pak'):
            self.assertTrue(tc.FILE_PATH.match(good), good)
        for bad in ('../x', '/etc/passwd', 'archives/../../x', 'a\\b', 'C:/x', ''):
            self.assertFalse(tc.FILE_PATH.match(bad) and '..' not in bad, bad)

    def test_a_manifest_lists_every_file(self):
        with tempfile.TemporaryDirectory() as root:
            package(root)
            m = tc.manifest_of(root)
            self.assertEqual(m['format'], 1)
            self.assertEqual(sorted(f['path'] for f in m['tracks'][0]['files']),
                             ['archives/t929_neo_bowser_city.pak', 'archives/update3.pak', 'track.json'])


class _Quiet(SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


class MirrorTests(unittest.TestCase):
    """The files come from the manifest's mirrors (Cloudflare R2), then from the server that gave the list."""

    def serve(self, directory):
        server = ThreadingHTTPServer(('127.0.0.1', 0), partial(_Quiet, directory=str(directory)))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return f'http://127.0.0.1:{server.server_address[1]}/'

    def published(self, root, damaged=False):
        """A web folder: <id>/<files> and manifest.json, as the server's /content/."""
        source = Path(root) / 'source'
        folder = package(source)
        web = Path(root) / ('damaged' if damaged else 'web')
        shutil.copytree(folder, web / 'neo_bowser_city')
        if damaged:
            (web / 'neo_bowser_city' / 'archives' / 'update3.pak').write_bytes(b'y')
        return source, web

    def sync_with(self, root, server, mirrors):
        source, web = self.published(root)
        manifest = tc.manifest_of(source)
        manifest['mirrors'] = mirrors
        (web / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
        settings = Path(root) / 'bbport.ini'
        settings.write_text(f'content_url={server(web)}\n', encoding='utf-8')
        out, said = Path(root) / 'got', []
        changed = tc.sync(settings, out, log=said.append)
        return out, said, changed, web

    def test_a_mirror_is_used_first(self):
        with tempfile.TemporaryDirectory() as root:
            _, mirror_web = self.published(Path(root) / 'm')
            mirror = self.serve(mirror_web)
            out, said, changed, _ = self.sync_with(root, self.serve, [mirror])
            self.assertEqual(changed, ['neo_bowser_city'])
            self.assertFalse(any('next source' in s for s in said), said)

    def test_a_dead_or_damaged_mirror_falls_back_to_the_server(self):
        with tempfile.TemporaryDirectory() as root:
            _, bad_web = self.published(Path(root) / 'b', damaged=True)
            mirrors = ['http://127.0.0.1:9/', self.serve(bad_web)]
            out, said, changed, _ = self.sync_with(root, self.serve, mirrors)
            self.assertEqual(changed, ['neo_bowser_city'])
            self.assertTrue(any('arrived damaged' in s for s in said), said)
            self.assertEqual((out / 'tracks' / 'neo_bowser_city' / 'archives' / 'update3.pak').read_bytes(), b'x')

    def test_mirrors_work_with_the_server_down(self):
        with tempfile.TemporaryDirectory() as root:
            _, mirror_web = self.published(Path(root) / 'm')
            mirror = self.serve(mirror_web)
            source = Path(root) / 'source'
            package(source)
            manifest = tc.manifest_of(source, [mirror])
            out = Path(root) / 'got'
            out.mkdir()
            (out / tc.MANIFEST_COPY).write_text(json.dumps(manifest), encoding='utf-8')   # the last list had
            settings = Path(root) / 'bbport.ini'
            settings.write_text('content_url=http://127.0.0.1:9/\n', encoding='utf-8')
            self.assertEqual(tc.sync(settings, out, log=lambda s: None), ['neo_bowser_city'])

    def test_only_web_addresses_are_mirrors(self):
        m = {'mirrors': ['https://pub-1.r2.dev/', 'file:///etc/', 'https://x/../', 'ftp://a/', 'https://cdn.example.com/c/']}
        self.assertEqual(tc.sources(m, 'http://1.2.3.4:8094/content/'),
                         ['https://pub-1.r2.dev/', 'https://cdn.example.com/c/', 'http://1.2.3.4:8094/content/'])
        with self.assertRaises(ValueError):
            tc.manifest_of(tempfile.gettempdir() + '/none', ['file:///x/'])


class HiddenFileTests(unittest.TestCase):
    def test_hidden_entries_leave_a_sorted_index(self):
        import struct
        import archive_patch
        names = ['shaders/standard.igz', 'maps/x.igz', 'loosedata/presets/cutscenepresets.igz']
        hashes = sorted(archive_patch.path_hash(n) for n in names)
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'level.pak'
            header = bytearray(tc.INDEX)
            struct.pack_into('<I', header, 0xc, len(hashes))
            path.write_bytes(bytes(header) + struct.pack(f'<{len(hashes)}I', *hashes))
            ranges = tc.hidden_ranges(path, names[0::2])
            for offset, data in ranges:
                hashes[(offset - tc.INDEX) // 4], = struct.unpack('<I', data)
            self.assertEqual(len(ranges), 2)
            self.assertEqual(hashes, sorted(hashes))
            self.assertNotIn(archive_patch.path_hash(names[0]), hashes)
            self.assertNotIn(archive_patch.path_hash(names[2]), hashes)
            self.assertIn(archive_patch.path_hash(names[1]), hashes)


class PackageTests(unittest.TestCase):
    def test_a_package_is_found(self):
        with tempfile.TemporaryDirectory() as root:
            package(root)
            found = tc.packages(root)
            self.assertEqual([m['id'] for _f, m in found], ['neo_bowser_city'])
            self.assertEqual(tc.zone(found[0][1]), 't929_neo_bowser_city')
            self.assertEqual([p.as_posix() for p, _s in tc.level_archives(root)],
                             ['archives/t929_neo_bowser_city.pak'])

    def test_broken_packages_are_skipped(self):
        for changes in ({'level': 'crash_cove'}, {'id': '../x'}, {'archives': ['archives/missing.pak']},
                        {'archives': ['../outside.pak']}):
            with tempfile.TemporaryDirectory() as root:
                package(root, **changes)
                self.assertEqual(tc.packages(root), [], changes)

    def test_no_content_no_entries(self):
        with tempfile.TemporaryDirectory() as root:
            self.assertEqual(tc.entries(root, root), [])
            self.assertEqual(tc.packages(root), [])


if __name__ == '__main__':
    unittest.main()
