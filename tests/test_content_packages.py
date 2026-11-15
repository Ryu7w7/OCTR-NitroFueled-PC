"""External content: content_package.py (the format, the custom_content folder, the choices) and
content_build.py (the update archive with the tracks registered), on a game made of the synthetic
files of test_track_registry.py."""
import hashlib
import json
import shutil
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace

import paths  # noqa: F401
import content_build as cb
import content_package as cp
import test_track_registry as ttr
import track_registry as tr
from graphics_tab import Archive, write_archive
from igz_merge import File
from mod_components import plain_path

ID = 't922_suzuka'
MAP = f'octane/{ID}/{ID}'
TEXTURE = ttr.TEXTURE
ZONE = f'maps/octane/{ID}/{ID}_zoneinfo.igz'


def fake_pak(names=('maps/x.igz',)):
    """A small native archive."""
    with tempfile.TemporaryDirectory() as folder:
        dest = Path(folder) / 'a.pak'
        write_archive(dest, [('temporary/' + n, n, i << 8, b'data ' + n.encode()) for i, n in enumerate(names)],
                      SimpleNamespace(flags=0))
        return dest.read_bytes()


def write_package(folder, ident=ID, files=None, manifest=None, image=True):
    """A content package folder; files: {relative path: bytes}."""
    folder = Path(folder)
    files = files if files is not None else {
        f'archives/{ident}.pak': fake_pak(), f'archives/{ident}_flyby.pak': fake_pak(),
        f'files/maps/octane/{ident}/{ident}_zoneinfo.igz': b'zone',
        **({f'files/textures/{TEXTURE}.igz': b'picture'} if image else {}),
        'README.txt': b'Suzuka'}
    for rel, data in files.items():
        (folder / rel).parent.mkdir(parents=True, exist_ok=True)
        (folder / rel).write_bytes(data)
    (folder / cp.MARKER).write_text(cp.MARKER_TEXT)
    m = {'format': 1, 'type': 'track', 'id': ident, 'name': {'en': 'Suzuka'}, 'version': '0.7.2',
         'author': 'CTRNFModding', 'map': f'octane/{ident}/{ident}', 'online': 'offline-only',
         'files': [{'file': rel, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}
                   for rel, data in sorted(files.items())]}
    if image:
        m.update(image='LevelImage_Suzuka', texture=TEXTURE)
    m.update(manifest or {})
    (folder / cp.MANIFEST).write_text(json.dumps(m))
    return m


def zip_folder(folder, dest):
    with zipfile.ZipFile(dest, 'w') as z:
        for p in sorted(Path(folder).rglob('*')):
            if p.is_file():
                z.write(p, p.relative_to(folder).as_posix())
    return dest


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

    def test_a_package_reads_back(self):
        m = write_package(self.dir / 'p')
        self.assertEqual(cp.read_folder(self.dir / 'p'), m)
        self.assertEqual([t for t, _ in cp.archives(m, self.dir / 'p')],
                         [f'archives/{ID}.pak', f'archives/{ID}_flyby.pak'])
        track = cp.track(m)
        self.assertEqual((track.map, track.image, track.texture, track.page), (MAP, 'LevelImage_Suzuka', TEXTURE, None))
        # The picture's texture is the track menu's (its LevelImage material), never the streaming
        # list's too: two owners there crash the game when it unloads.
        self.assertEqual(sorted(track.files), [('igx_file', ZONE)])
        streamed = dict(m, stream=[f'textures/{TEXTURE}.igz'])  # a converted mod's own list wins
        self.assertEqual(cp.track(streamed).files, [('texture', f'textures/{TEXTURE}.igz')])

    def test_bad_manifests_are_refused(self):
        base = write_package(self.dir / 'p')
        cases = {
            'unknown fields': {'script': 'x'},
            'format must be': {'format': 2},
            'type must be': {'type': 'mod'},
            'map must be': {'map': 'octane/t111_crash_cove/t111_crash_cove'},
            'online must be': {'online': 'public'},
            'image and texture go together': {'texture': None},
            'at least an "en" name': {'name': {'es': 'Suzuka'}},
        }
        for message, change in cases.items():
            m = {k: v for k, v in {**base, **change}.items() if v is not None}
            with self.subTest(message), self.assertRaisesRegex(ValueError, message):
                cp.validate_manifest(m)

    def test_files_a_package_cannot_have(self):
        for path, message in ((f'archives/other.pak', 'archives are archives/t922_suzuka'),
                              ('archives/update3.pak', 'archives are archives'),
                              (f'archives/{ID}_update3.pak', None),
                              ('files/patch.xml', 'not a game asset'),
                              ('patches/a.xml', 'content packages hold'),
                              ('../escape.pak', 'Unsafe path')):
            with self.subTest(path):
                files = {f'archives/{ID}.pak': b'x', path: b'x'}
                m = {'format': 1, 'type': 'track', 'id': ID, 'name': {'en': 'S'}, 'version': '1',
                     'author': 'a', 'map': MAP,
                     'files': [{'file': p, 'sha256': '0' * 64, 'bytes': 1} for p in files]}
                if message is None:
                    cp.validate_manifest(m)
                else:
                    with self.assertRaisesRegex(ValueError, message):
                        cp.validate_manifest(m)
        with self.assertRaisesRegex(ValueError, 'cannot ship update archives'):
            cp.kind_of(f'archives/{ID}.pak'.replace(f'{ID}.pak', 'update3.pak'), 'update')

    def test_the_files_must_match_the_inventory(self):
        write_package(self.dir / 'p')
        (self.dir / 'p/extra.png').write_bytes(b'x')
        with self.assertRaisesRegex(ValueError, 'not listed extra.png'):
            cp.read_folder(self.dir / 'p')
        (self.dir / 'p/extra.png').unlink()
        (self.dir / f'p/files/maps/octane/{ID}/{ID}_zoneinfo.igz').write_bytes(b'ZONE')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            cp.read_folder(self.dir / 'p')
        (self.dir / 'p' / cp.MARKER).unlink()
        with self.assertRaisesRegex(ValueError, 'missing octr-content.required'):
            cp.read_folder(self.dir / 'p')

    def test_streaming_kinds_follow_the_game_s(self):
        self.assertEqual([cp.stream_kind(p) for p in ('textures/a.igz', 'materialinstances/ui/a.igz', 'maps/a/b.igz',
                                                      'loosedata/a/b.igz', 'vsc/a.igz', 'maps/a/b_en.lng')],
                         ['texture', 'material_instances', 'igx_file', 'igx_file', 'vsc', 'lang_file'])


class FolderTests(unittest.TestCase):
    def setUp(self):
        self.port = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.port, ignore_errors=True)
        self.root = self.port / cp.FOLDER
        self.root.mkdir()

    def logs(self):
        out = []
        return out, out.append

    def test_zips_are_unpacked_once_and_folders_read_in_place(self):
        write_package(self.port / 'work/suzuka')
        zip_folder(self.port / 'work/suzuka', self.root / 'suzuka.zip')
        write_package(self.root / 'nbc', ident='t929_neo_bowser_city', image=False)
        found = cp.scan(self.root, log=lambda _: None)
        self.assertEqual(found['messages'], [])
        self.assertEqual(sorted(found['packages']), [ID, 't929_neo_bowser_city'])
        unpacked = Path(found['packages'][ID]['folder'])
        self.assertTrue(plain_path(unpacked).is_relative_to(plain_path(cp.state(self.root))))
        marker = unpacked / 'README.txt'
        marker.write_bytes(b'kept')  # a second scan does not unpack again
        cp.scan(self.root, log=lambda _: None)
        self.assertEqual(marker.read_bytes(), b'kept')

    def test_broken_packages_are_named_and_skipped(self):
        (self.root / 'broken.zip').write_bytes(b'not a zip')
        write_package(self.root / 'a')
        write_package(self.root / 'b')  # the same id twice
        messages, log = self.logs()
        found = cp.scan(self.root, log=log)
        self.assertEqual(list(found['packages']), [ID])
        self.assertEqual(len(found['messages']), 2)
        self.assertTrue(all(m.startswith('External content: ') for m in messages))

    def test_selection_turns_packages_or_everything_off(self):
        write_package(self.root / 'suzuka')
        write_package(self.root / 'nbc', ident='t929_neo_bowser_city', image=False, manifest={'online': 'private'})
        quiet = {'log': lambda _: None}
        self.assertEqual([m['id'] for m, _ in cp.selected(self.root, **quiet)], [ID, 't929_neo_bowser_city'])
        cp.save_selection(self.root, disable=[ID])
        self.assertEqual([m['id'] for m, _ in cp.selected(self.root, **quiet)], ['t929_neo_bowser_city'])
        cp.save_selection(self.root, enabled=False, enable=[ID])
        self.assertEqual(cp.selected(self.root, **quiet), [])
        cp.save_selection(self.root, enabled=True, page='bonus')
        self.assertEqual(cp.selection(self.root), {'enabled': True, 'disabled': [], 'page': 'bonus'})
        with self.assertRaisesRegex(ValueError, 'page must be'):
            cp.save_selection(self.root, page='first')

    def test_online_keeps_only_private_room_packages(self):
        write_package(self.root / 'suzuka')
        write_package(self.root / 'nbc', ident='t929_neo_bowser_city', image=False, manifest={'online': 'private'})
        messages, log = self.logs()
        self.assertEqual([m['id'] for m, _ in cp.selected(self.root, online=True, log=log)], ['t929_neo_bowser_city'])
        self.assertIn('External content: t922_suzuka is offline only; not loaded with Online on', messages)


def game_folder(root, select_blob=None):
    """A game: update2.pak with the registration files, a base archive."""
    entries = ttr.game()
    if select_blob is not None:
        entries[tr.SELECT] = select_blob
    archives = Path(root) / 'archives'
    archives.mkdir(parents=True)
    write_archive(archives / 'update2.pak',
                  [('temporary/' + p, p, i << 8, d) for i, (p, d) in enumerate(entries.items())],
                  SimpleNamespace(flags=0))
    write_archive(archives / 't111_crash_cove.pak', [('t/maps/a.igz', 'maps/octane/t111_crash_cove/a.igz', 0, b'a')],
                  SimpleNamespace(flags=0))
    return entries


def read(path):
    a = Archive(Path(path))
    try:
        return {a.logical(i): a.read(i) for i in range(len(a.names))}
    finally:
        a.f.close()


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.game = self.dir / 'game'
        self.entries = game_folder(self.game)
        self.staging = self.dir / 'staging'
        self.staging.mkdir()
        self.store = self.dir / 'state'

    def package(self, name='suzuka', **kw):
        folder = self.dir / name
        m = write_package(folder, **kw)
        return (m, folder)

    def build(self, packages, operations=(), own_page=True):
        self.staging.mkdir(exist_ok=True)
        own = cb.build(self.game, self.store, self.staging, packages, list(operations), {'graphics_tab': '0'},
                       log=lambda _: None, own_page=own_page)
        return own, read(self.staging / 'files/archives/update3.pak')

    def test_the_update_archive_registers_the_tracks_and_holds_their_files(self):
        suzuka = self.package()
        nbc = self.package('nbc', ident='t929_neo_bowser_city', image=False)
        own, archive = self.build([suzuka, nbc])
        self.assertEqual([t.as_posix() for t, _ in own],
                         [f'archives/{ID}.pak', f'archives/{ID}_flyby.pak',
                          'archives/t929_neo_bowser_city.pak', 'archives/t929_neo_bowser_city_flyby.pak'])
        self.assertEqual(archive[ZONE], b'zone')
        self.assertEqual(archive[f'textures/{TEXTURE}.igz'], b'picture')
        layout = tr.pages(archive[tr.GUI], archive[tr.SELECT])
        self.assertEqual(layout[-1][2], [MAP, 'octane/t929_neo_bowser_city/t929_neo_bowser_city'])
        self.assertEqual(ttr.page_titles(archive[tr.LANGUAGE_FILE.format('en')])[tr.OWN_LIST], 'External Tracks')
        self.assertIn('LevelImage_Suzuka', tr.named(File(archive[tr.MATERIALS])))

    def test_on_the_bonus_page_no_page_is_added(self):
        _, archive = self.build([self.package()], own_page=False)
        layout = tr.pages(archive[tr.GUI], archive[tr.SELECT])
        self.assertEqual(layout[-1][1], tr.POPULATOR + '6')
        self.assertEqual(layout[-1][2][-1], MAP)
        self.assertNotIn(tr.LANGUAGE_FILE.format('en'), archive)

    def test_a_mod_s_change_to_the_track_menu_is_kept(self):
        """Custom Cup changes the track menu inside update2.pak: the tracks go into its version."""
        original, patched = self.entries[tr.SELECT], ttr.select(extra=True)
        (self.dir / 'mod/payloads').mkdir(parents=True)
        (self.dir / 'mod/payloads/menu.igz').write_bytes(patched)
        op = {'kind': 'archive_asset', 'archive': 'update2.pak', 'asset': 'ui/Octane_LevelSelect_Proto.igz',
              'original_sha256': hashlib.sha256(original).hexdigest(),
              'patched_sha256': hashlib.sha256(patched).hexdigest(), 'payload': 'payloads/menu.igz'}
        _, archive = self.build([self.package()], [(op, self.dir / 'mod')])
        menu = tr.named(File(archive[tr.SELECT]))
        self.assertIn('CustomCup_Thumbnail', menu)
        self.assertEqual(tr.pages(archive[tr.GUI], archive[tr.SELECT])[-1][2], [MAP])

    def test_content_cannot_replace_game_files(self):
        replacing = self.package(files={f'archives/{ID}.pak': b'x', 'files/maps/octane/t111_crash_cove/a.igz': b'x'},
                                 image=False)
        with self.assertRaisesRegex(ValueError, 'is a game file'):
            self.build([replacing])
        shadow = self.package('shadow', ident='t111_crash_cove', files={'archives/t111_crash_cove.pak': b'x'},
                              image=False)
        with self.assertRaisesRegex(ValueError, 'is a game archive'):
            self.build([shadow])

    def test_shared_files_are_the_game_s_own_bytes(self):
        # A game file the track needs loaded with it (in the game, only in another track's archive).
        game_copy = 'maps/octane/t111_crash_cove/a.igz'
        a = self.package('a', files={f'archives/{ID}.pak': b'x', 'shared/' + game_copy: b'a'}, image=False)
        b = self.package('b', ident='t929_neo_bowser_city',
                         files={'archives/t929_neo_bowser_city.pak': b'x', 'shared/' + game_copy: b'a'}, image=False)
        _, update = self.build([a, b])  # two packages may carry the same copy
        self.assertEqual(update[game_copy], b'a')
        # Found by path, not streamed with the track: no streaming list gets it (nothing else to add).
        self.assertNotIn(tr.CHUNKS, update)
        changed = self.package('c', files={f'archives/{ID}.pak': b'x', 'shared/' + game_copy: b'changed'}, image=False)
        with self.assertRaisesRegex(ValueError, 'not an unchanged copy'):
            self.build([changed])
        streamed = self.package('e', files={f'archives/{ID}.pak': b'x', 'shared/' + game_copy: b'a'}, image=False,
                                manifest={'stream': [game_copy]})
        _, update = self.build([streamed])  # as the converted mod had it: streamed with the track
        self.assertIn(game_copy, {p for _, p in tr.chunk_entries(File(update[tr.CHUNKS]))[1]})
        new = self.package('d', files={f'archives/{ID}.pak': b'x', 'shared/maps/new.igz': b'n'}, image=False)
        with self.assertRaisesRegex(ValueError, 'not a game file; put it in files/'):
            self.build([new])

    def test_two_packages_cannot_share_a_file(self):
        a = self.package('a', files={f'archives/{ID}.pak': b'x', 'files/loosedata/shared.igz': b'x'}, image=False)
        b = self.package('b', ident='t929_neo_bowser_city',
                         files={'archives/t929_neo_bowser_city.pak': b'x', 'files/loosedata/shared.igz': b'y'},
                         image=False)
        with self.assertRaisesRegex(ValueError, 'both have loosedata/shared.igz'):
            self.build([a, b])

    def test_no_packages_build_nothing(self):
        self.assertEqual(cb.build(self.game, self.store, self.staging, [], [], {}), [])
        self.assertFalse((self.staging / 'files').exists())

    def test_the_game_index_is_cached(self):
        index = cp.game_index(self.game, self.store)
        self.assertIn('ui/octane_levelselect_proto.igz', index['paths'])
        self.assertIn('t111_crash_cove.pak', index['archives'])
        self.assertEqual(len(list((self.store / 'game-index').glob('*.json'))), 1)
        self.assertEqual(cp.game_index(self.game, self.store), index)


if __name__ == '__main__':
    unittest.main()


class ConvertTests(unittest.TestCase):
    """content_convert.py: a classic track mod (its archives and a whole update3.pak) to a package."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.game = self.dir / 'game'
        self.entries = game_folder(self.game)

    def classic(self, image=True, extra_track=False):
        import content_convert  # noqa: F401
        picture = 'LevelImage_T922_Suzuka' if extra_track else 'LevelImage_Suzuka'  # named after its track
        tracks = [tr.Track(MAP, image=picture if image else None, texture=TEXTURE if image else None)]
        if extra_track:
            tracks.append(tr.Track('octane/t929_neo_bowser_city/t929_neo_bowser_city'))
        registered = {**self.entries, **tr.register(dict(self.entries), tracks, log=lambda _: None)}
        update3 = dict(registered)  # every registration file, changed or not (filler)
        update3[ZONE] = b'zone'
        update3['maps/octane/t111_crash_cove/a.igz'] = b'changed'  # a global change
        if image:
            update3[f'textures/{TEXTURE}.igz'] = b'picture'
        if extra_track:
            update3['maps/octane/t929_neo_bowser_city/t929_neo_bowser_city_zoneinfo.igz'] = b'zone 2'
        mod = self.dir / 'mod'
        (mod / 'archives').mkdir(parents=True)
        write_archive(mod / 'archives/update3.pak',
                      [('t/' + p, p, i << 8, d) for i, (p, d) in enumerate(update3.items())], SimpleNamespace(flags=0))
        (mod / 'archives' / f'{ID}.pak').write_bytes(fake_pak())
        if extra_track:
            (mod / 'archives' / 't929_neo_bowser_city.pak').write_bytes(fake_pak())
        (mod / 'mod.json').write_text(json.dumps({'id': 'suzuka', 'name': 'Suzuka (F1)', 'version': '0.7.2',
                                                  'author': 'CTRNFModding', 'compatibility': {'online': 'offline-only'}}))
        (mod / 'README.txt').write_text('Suzuka')
        zip_folder(mod, self.dir / 'suzuka.zip')
        return self.dir / 'suzuka.zip'

    def test_a_classic_track_mod_becomes_a_package_without_its_global_changes(self):
        import content_convert
        [dest], dropped = content_convert.convert(self.game, self.classic(), self.dir / 'out', log=lambda _: None)
        self.assertEqual(dropped, ['maps/octane/t111_crash_cove/a.igz'])
        m = cp.read_folder(dest)
        self.assertEqual((m['id'], m['map'], m['image'], m['texture'], m['online']),
                         (ID, MAP, 'LevelImage_Suzuka', TEXTURE, 'offline-only'))
        self.assertEqual((m['name'], m['version'], m['author']), ({'en': 'Suzuka (F1)'}, '0.7.2', 'CTRNFModding'))
        self.assertEqual(m['convertedFrom']['left_out'], dropped)
        self.assertIn('CTRNFModding', m['credits'])
        self.assertEqual(sorted(r['file'] for r in m['files']),
                         sorted(['README.txt', f'archives/{ID}.pak', f'files/{ZONE}', f'files/textures/{TEXTURE}.igz']))
        # The package registers the same track again.
        own, archive = BuildTests.build(SimpleNamespace(game=self.game, store=self.dir / 'state',
                                                        staging=self.dir / 'staging'), [(m, dest)])
        self.assertEqual(tr.pages(archive[tr.GUI], archive[tr.SELECT])[-1][2], [MAP])

    def test_a_mod_with_two_new_tracks_becomes_two_packages(self):
        import content_convert
        made, _ = content_convert.convert(self.game, self.classic(extra_track=True), self.dir / 'out',
                                          log=lambda _: None)
        first, second = (cp.read_folder(d) for d in made)
        self.assertEqual((first['id'], first['image'], second['id']), (ID, 'LevelImage_T922_Suzuka', 't929_neo_bowser_city'))
        self.assertNotIn('image', second)
        self.assertEqual(sorted(r['file'] for r in first['files']),
                         sorted(['README.txt', f'archives/{ID}.pak', f'files/{ZONE}', f'files/textures/{TEXTURE}.igz']))
        self.assertEqual(sorted(r['file'] for r in second['files']),
                         ['README.txt', 'archives/t929_neo_bowser_city.pak',
                          'files/maps/octane/t929_neo_bowser_city/t929_neo_bowser_city_zoneinfo.igz'])
        # no zone info names it: the mod's name and the track id
        self.assertEqual(second['name'], {'en': 'Suzuka (F1) (t929_neo_bowser_city)'})
        # both build together
        BuildTests.build(SimpleNamespace(game=self.game, store=self.dir / 'state', staging=self.dir / 'staging'),
                         [(first, made[0]), (second, made[1])])

    def test_tracks_exported_together_are_told_to_be_exported_apart(self):
        import contextlib, io, content_cli
        for extra, root in ((False, 'one'), (True, 'two')):
            shutil.rmtree(self.dir / 'mod', ignore_errors=True)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = content_cli.main([str(self.dir / root), 'convert', str(self.game),
                                         str(self.classic(extra_track=extra))])
            self.assertEqual(code, 0, out.getvalue())
            # several tracks in one mod share textures (road, minimap): partial together, export apart
            self.assertEqual('exported together' in out.getvalue(), extra, out.getvalue())


class WholeUpdate3Tests(unittest.TestCase):
    """mod_content.whole_update3_message: a mod with a whole update3.pak stops external content;
    only a track mod is told to convert it."""

    def test_only_track_mods_are_told_to_convert(self):
        import mod_content
        work = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, work, ignore_errors=True)
        track, racer = work / 'track.pak', work / 'racer.pak'
        write_archive(track, [('t/' + tr.GUI, tr.GUI, 0, b'gui')], SimpleNamespace(flags=0))
        write_archive(racer, [('t/looseData/systemData.igz', 'looseData/systemData.igz', 0, b'sys')],
                      SimpleNamespace(flags=0))
        self.assertTrue(mod_content.registers_tracks(track))
        self.assertFalse(mod_content.registers_tracks(racer))
        self.assertFalse(mod_content.registers_tracks(work / 'missing.pak'))
        message = mod_content.whole_update3_message(['nbc', 'tag'], {'nbc': track, 'tag': racer})
        self.assertIn('nbc ships a whole update3.pak: turn it off, or convert it to external content', message)
        self.assertIn('tag ships a whole update3.pak, which cannot be used with external content', message)
        self.assertNotIn('convert', message.split('; ')[1])
