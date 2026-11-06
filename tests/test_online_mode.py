"""Native menu adapter checks; BB_TEST_GAME enables tests using a local game dump."""
from paths import ROOT
import os
import json
import struct
import subprocess
from pathlib import Path
import unittest
import tempfile
from unittest.mock import MagicMock, patch

import gcn_shaders as iga
from igz_file import Igz, unpack_offsets
import online_mode
from prepare import parse_self


class OnlineModeTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires native game dump')
    def test_ranked_button_below_matchmaking(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update2.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower() == online_mode.MAIN_LOBBY_ENTRY)
        with pak.open('rb') as f:
            blob = iga.read_entry(f, *entry[1:])
        old, new = Igz(blob), Igz(online_mode.ranked_main_button(blob))
        button = next(o for o, t in new.objects() if o >= len(old.data) and
                      t == 'igGuiPlaceable')
        self.assertEqual(new.string_at(button + 0x28), 'OCTR_RANKED_BUTTON')
        self.assertEqual(new.u64(0x970 + 0x50), button)
        self.assertEqual(new.u64(button + 0x50), 0xa90)
        self.assertEqual(new.u64(0xa90 + 0x58), button)
        self.assertEqual(new.u32(0x900 + 12), 9)
        self.assertEqual(dict(new.objects())[new.u64(button + 0xe8)], 'igGuiBehaviorList')
        # The ranked button shares the matchmaking VSC behavior object (one graph instance,
        # refcounted by both lists); the clone of that object is never instantiated.
        behaviors = new.u64(button + 0xe8)
        self.assertEqual(new.u32(behaviors + 0xc), 2)
        self.assertEqual(new.u64(new.u64(behaviors + 0x20) + 8), 0x23b8)
        self.assertNotIn(button + 0x3c8, new.sites['RVTB'])  # the cloned VSC (0x2110+0x2a8)
        self.assertEqual(struct.unpack_from('<f', new.data, 0x970 + 0x64)[0], -315.0)
        self.assertEqual(struct.unpack_from('<f', new.data, button + 0x64)[0], -210.0)
        # A distinct icon, as the native Graphics-tab Restart button: the navigation's icon
        # material fields (+0x158 with its +0x178/+0x1a8 mirrors) get the ready-button material
        # id, and the focused icon sprite gets its name reference at +0x150.
        behaviors = new.u64(button + 0xe8)
        nav = next(new.u64(s) for s in new.sites['ROFS'] if behaviors <= s < behaviors + 0x40
                   and dict(new.objects()).get(new.u64(s)) == 'CGuiBehaviorOctaneNavigationItem')
        exid = new.fixup('EXID')
        self.assertEqual((new.u64(nav + 0x158), new.u64(nav + 0x178), new.u64(nav + 0x1a8)),
                         (18,) * 3)  # the grand prix button's material, already in this file
        material_id, namespace_id = struct.unpack_from('<II', exid[3], 18 * 8)
        self.assertEqual(material_id, 0x7c593110)  # 'octane_ui_ico_grandprix_tga__material'
        self.assertEqual(namespace_id, 0x41e2d304)
        self.assertIn(nav + 0x150, new.read_list('RNEX', unpack_offsets))
        exnm_entries = struct.iter_unpack('<II', new.fixup('EXNM')[3][:new.fixup('EXNM')[1] * 8])
        self.assertEqual(new.u64(nav + 0x150),
                         sum(1 for _, ns in exnm_entries if not ns & 0x80000000) - 1)

    def test_cup_package_regional_languages(self):
        graph = 'ui/octane_cupselect.igz'
        english = 'ui/octane_cupselect_en.lng'
        portuguese = 'ui/octane_cupselect_pt.lng'
        texture = 'textures/cup.igz'
        rows = [('gui_project', graph), ('lang_file', english),
                ('lang_file', portuguese), ('texture', texture)]
        dependencies = b''.join(kind.encode() + b'\0' + path.encode() + b'\0'
                                for kind, path in rows[1:])
        for missing in (None, portuguese, english, texture):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as temp:
                archives = Path(temp) / 'archives'
                archives.mkdir()
                for name in ('juicedomain_startscreen', 'update', 'update2'):
                    (archives / (name + '.pak')).touch()
                entries = [(online_mode.PACKAGE_PREFIX + path, 0)
                           for _, path in rows if path != missing]
                package, source, asset = MagicMock(), MagicMock(), MagicMock()
                package.append.return_value = 0
                package.string_index.side_effect = lambda value: rows.index(
                    next(row for row in rows if value in row))
                package.u64.return_value = 0
                asset.fixup.return_value = ('TDEP', 3, 0, dependencies)
                with patch.object(online_mode, 'Igz', side_effect=[package, source] + [asset] * 4), \
                        patch.object(online_mode, 'package_rows', side_effect=[(0, []), (0, rows)]), \
                        patch.object(iga, 'pak_entries', return_value=entries), \
                        patch.object(iga, 'read_entry', return_value=b'asset'):
                    if missing in (english, texture):
                        with self.assertRaises(KeyError):
                            online_mode.cup_package(temp, b'online', b'source')
                    else:
                        online_mode.cup_package(temp, b'online', b'source')
                        included = [call.args[0] for call in package.string_index.call_args_list]
                        for _, path in rows:
                            self.assertEqual(path in included, path != missing)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local USA/EU 1.21 game dump')
    def test_prepare_preserves_widescreen_lobby(self):
        import archive_patch
        from nitro_components import payload_bytes
        game = Path(os.environ['BB_TEST_GAME'])
        pak = game / 'archives/update2.pak'
        entries = iga.pak_entries(pak)
        index = next(i for i, entry in enumerate(entries)
                     if entry[0].lower() == online_mode.LOBBY_ENTRY)
        original = online_mode.current_entry(pak, entries, index, None)
        data = ROOT / 'scripts/widescreen_data'
        tested = 0
        for manifest in (data / 'ultrawide-21x9/nitro-mod.json',
                         data / 'ultrawide-32x9/nitro-mod.json', data / 'eu/assets.json'):
            spec = json.loads(manifest.read_text())
            for name, operations in spec.get('sets', spec.get('operationSets', {})).items():
                for op in operations:
                    if (op.get('archive') != 'update2.pak' or op.get('index') != index
                            or op.get('asset', '').lower() != 'ui/octane_lobby_privategame.igz'):
                        continue
                    with self.subTest(manifest=manifest.name, variant=name), tempfile.TemporaryDirectory() as temp:
                        out = Path(temp)
                        wide = payload_bytes(op, manifest.parent, original)
                        base = out / 'widescreen.bbpatch'
                        archive_patch.write_entry_patch(base, pak, '/app0/archives/update2.pak', index, wide)
                        elf = out / 'eboot.elf'
                        elf.write_bytes(b'')
                        with patch.object(online_mode, 'hook_site', return_value=0x1277e00):
                            patches, _ = online_mode.prepare(game, out / 'online', elf, [base])
                        result = next(p for p in patches if archive_patch.read(p)[1] == '/app0/archives/update2.pak')
                        actual = online_mode.current_entry(pak, entries, index, result)
                        self.assertEqual(actual, online_mode.lobby_card(wide))
                        self.assertNotEqual(actual, online_mode.lobby_card(original))
                        tested += 1
        self.assertGreater(tested, 0)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
    def test_team_slot_ready_tick_is_name_child(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower().endswith('/octane_teamselect.igz'))
        with pak.open('rb') as f:
            g = Igz(iga.read_entry(f, *entry[1:]))
        slots = [o for o, kind in g.objects() if kind == 'CGuiBehaviorOctaneTeamSelectPlayerSlot']
        self.assertTrue(slots)
        for slot in slots:
            name = g.u64(slot + 0x10)
            children = g.u64(name + 0x48)
            self.assertEqual(g.u32(children + 12), 1)
            tick = g.u64(g.u64(children + 0x20))
            self.assertEqual(g.u64(tick + 0x40), name)
            self.assertEqual(struct.unpack_from('<f', g.data, tick + 0x70)[0], .5)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
    def test_team_selector_ready_unready_legends(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower().endswith('/octane_teamselect_root.igz'))
        with pak.open('rb') as f:
            old = Igz(iga.read_entry(f, *entry[1:]))
        new = Igz(online_mode.team_selector_legend(old.build()))
        changed = []
        for node, kind in old.objects():
            if kind != 'igVscCppActionNode' or old.u32(node + 0x10) == new.u32(node + 0x10):
                continue
            changed.append(node)
            button = old.u32(old.u64(old.u64(node + 0x20) + 8) + 16)
            self.assertEqual(new.u32(node + 0x10), 6 if button == 1 else 5)
            a, b = old.u64(node + 0x20), new.u64(node + 0x20)
            self.assertEqual(old.data[a:a+32], new.data[b:b+32])
            self.assertIn(new.u32(new.u64(b+8)+16), (1, 2))
            self.assertEqual(new.u64(b+32), 0)
        self.assertEqual(len(changed), 2)
        elf = parse_self((Path(os.environ['BB_TEST_GAME']) / 'eboot.bin').read_bytes())[0]
        shift = 0x1277e00 - online_mode.hook_site(elf)
        from patches import load_segments
        v, off, size = load_segments(elf)[0]
        at = off + 0x110bbc0 - shift - v
        self.assertEqual(elf[at:at+17].hex(), '554889e54157415641554154534883ec18')

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
    def test_private_start_button_marks_only_the_pre_start_guard(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update.pak'
        entries = iga.pak_entries(pak)
        entry = next(e for e in entries if e[0].lower().endswith('/octane_lobby_privategame_buttonready.igz'))
        with pak.open('rb') as f:
            original = Igz(iga.read_entry(f, *entry[1:]))
        patched = Igz(online_mode.private_start_button(original.build()))
        x = patched.fixup('EXNM')
        methods = [patched.strings[a] for a, b in struct.iter_unpack('<II', x[3][:x[1] * 8])
                   if not b & 0x80000000]
        old_actions = {o for o, t in original.objects() if t == 'igVscCppActionNode'}
        added = [o for o, t in patched.objects() if t == 'igVscCppActionNode' and o not in old_actions]
        self.assertEqual(len(added), 2)
        native_functions = patched.read_list('RNEX', unpack_offsets)
        for node in added:
            self.assertIn(node + 0x38, native_functions)
            self.assertIn(node + 0x18, patched.sites['RPID'])

        guard = next(o for o in added if patched.u32(o + 0x10) == 1)
        host = next(o for o in added if o != guard)
        self.assertEqual(methods[patched.u64(guard + 0x38)], 'COctaneLobbyNodes::canStartGame_VSC')
        self.assertEqual(methods[patched.u64(host + 0x38)], 'CNetworkNodes::isHost2_VSC')
        self.assertEqual(patched.u64(patched.u64(guard + 0x20)), 0)
        def outputs(node):
            data = patched.u64(patched.u64(node + 0x30) + 0x20)
            return patched.u64(data), patched.u64(data + 8)
        ready, rejected = outputs(guard)
        self.assertEqual(rejected, 0)
        self.assertEqual(methods[patched.u64(ready + 0x38)], 'COctaneLobbyNodes::readyPlayerLobby_VSC')
        self.assertEqual(outputs(host), (guard, ready))  # Guests can still ready normally.
        rewired = []
        for node, kind in original.objects():
            if kind == 'igVscActionNodeList' and original.u32(node + 0x10) == 2:
                data = original.u64(node + 0x20)
                if original.u64(data + 8) == ready:
                    self.assertEqual(patched.u64(data + 8), host)
                    rewired.append(node)
            if node in old_actions:
                self.assertEqual(original.data[node:node + 0x40], patched.data[node:node + 0x40])
        self.assertEqual(len(rewired), 1)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
    def test_team_stings_mark_native_win_loss_only(self):
        for name in ('update.pak', 'update2.pak'):
            pak = Path(os.environ['BB_TEST_GAME']) / 'archives' / name
            entry = next(e for e in iga.pak_entries(pak)
                         if e[0].lower() == online_mode.RACE_MUSIC_ENTRY)
            with pak.open('rb') as f:
                original = Igz(iga.read_entry(f, *entry[1:]))
            patched = Igz(online_mode.team_finish_sounds(original.build()))
            exid = original.fixup('EXID')
            handles = list(struct.iter_unpack('<II', exid[3][:exid[1] * 8]))
            marked = []
            for node, kind in original.objects():
                if kind != 'igVscCppActionNode':
                    continue
                before, after = original.u32(node + 0x10), patched.u32(node + 0x10)
                self.assertEqual(original.u64(node + 0x28), patched.u64(node + 0x28))
                if before == after:
                    continue
                self.assertEqual((before, after), (4, 5))
                old_pins, new_pins = original.u64(node + 0x20), patched.u64(node + 0x20)
                self.assertEqual(original.data[old_pins:old_pins + 32], patched.data[new_pins:new_pins + 32])
                self.assertEqual(patched.u64(new_pins + 32), 0)
                accessor = original.u64(old_pins + 8)
                field = original.u64(accessor + 0x10)
                marked.append(handles[original.u64(field + 0x50)])
            self.assertCountEqual(marked, [(0x8305d2d3, 0x4305904b), (0x789c92ee, 0x4305904b)])
    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires native game dump')
    def test_public_vote_card_marker_and_binding_order(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update2.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower() == online_mode.PUBLIC_LOBBY_ENTRY)
        with pak.open('rb') as f:
            blob = iga.read_entry(f, *entry[1:])
        old, new = Igz(blob), Igz(online_mode.public_lobby_card(blob))
        self.assertEqual(new.string_at(0x5f0 + 0x28), 'OCTR_PUBLIC_LOBBY')
        self.assertEqual(new.string_at(0x2100 + 0x28), 'MapVoteResultModeName')
        self.assertEqual(new.string_at(0x51e0 + 0x28), '[ModeName]')
        def behaviors(g):
            seq = g.u64(0x5f0 + 0xe8)
            slots = g.u64(seq + 0x20)
            return [g.u64(slots + i * 8) for i in range(g.u32(seq + 12))]
        self.assertCountEqual(behaviors(old), behaviors(new))
        self.assertEqual(dict(new.objects())[behaviors(new)[-1]], 'CGuiBehaviorReturnToMainMenuOnDisconnect')

    def test_linux_launch_prepares_both_regions(self):
        script = (ROOT / 'run.sh').read_text()
        wrapper = script[script.index('startup_python() {'):].split('\n}\n', 1)[0] + '\n}\n'
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
boot=boot-cache/selected
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
                    ['bash', '-c', setup + wrapper + block +
                     '\nprintf "%s|%s|%s" "${BB_ONLINE_MODE_HOOK:-}" '
                     '"${BB_ARCHIVE_PATCHES:-}" "${BB_PRIVATE_ARCHIVE_PATCHES:-}"',
                     'test', title, version, online], capture_output=True, text=True, check=True)
                self.assertEqual(result.stdout,
                                 'test-hook|normal.bbpatch|speed.bbpatch' if enabled else '||')

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
    def test_streaming_ranking_archive_contains_patched_project_and_dependencies(self):
        from graphics_tab import Archive
        game = Path(os.environ['BB_TEST_GAME'])
        pak = game / 'archives/update2.pak'
        entries = iga.pak_entries(pak)
        with pak.open('rb') as f:
            entry = next(e for e in entries if e[0].lower().endswith('/juicedomain_race_pkg.igz'))
            package = iga.read_entry(f, *entry[1:])
            entry = next(e for e in entries if e[0].lower() == online_mode.TEAM_RANKING_ROOT)
            graph = online_mode.team_ranking_navigation(iga.read_entry(f, *entry[1:]))
        relative = online_mode.TEAM_RANKING_ROOT.removeprefix(online_mode.PACKAGE_PREFIX)
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / 'octane_battleteam_ranking.pak'
            online_mode.ranking_stream_archive(game, target, package, 'octane_battleteam_ranking',
                                               {relative: graph})
            archive = Archive(target)
            try:
                files = {archive.logical(i): archive.read(i) for i in range(len(archive.names))}
            finally:
                archive.f.close()
            self.assertEqual(files[relative], graph)
            self.assertIn('ui/octane_battleteam_ranking.igz', files)
            rows = online_mode.package_rows(Igz(files[
                'packages/generated/ui/octane_battleteam_ranking_pkg.igz']))[1]
            self.assertTrue(rows)
            self.assertTrue(all(path in files for _, path in rows))
            self.assertTrue(any(kind == 'material_instances' for kind, _ in rows))
            self.assertFalse(any('octane_cupselect' in path for _, path in rows))

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
    def test_team_ranking_navigation_marks_only_network_nodes(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update2.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower() == online_mode.TEAM_RANKING_ROOT)
        with pak.open('rb') as f:
            blob = iga.read_entry(f, *entry[1:])
        old, new = Igz(blob), Igz(online_mode.team_ranking_navigation(blob))
        changed = []
        for offset, kind in old.objects():
            size = old.sizes[old.u64(offset)]
            if old.data[offset:offset + size] != new.data[offset:offset + size]:
                changed.append(offset)
                self.assertEqual(kind, 'igVscCppActionNode')
                self.assertEqual(new.u32(offset + 0x10), 1)
                self.assertEqual(new.u64(new.u64(offset + 0x20)), 0)
                self.assertEqual(old.data[offset + 0x28:offset + size], new.data[offset + 0x28:offset + size])
        self.assertEqual(len(changed), 3)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
    def test_native_finish_time_requires_complete_laps(self):
        elf, _, headers, *_ = parse_self((Path(os.environ['BB_TEST_GAME']) / 'eboot.bin').read_bytes())
        def read(address, size):
            segment = next(p for p in headers if p['type'] in (1, 0x61000010) and
                           p['vaddr'] <= address < p['vaddr'] + p['filesz'])
            at = segment['offset'] + address - segment['vaddr']
            return elf[at:at + size]
        # Battle marker initialization flag and native per-frame event dispatch.
        self.assertEqual(read(0x9d6a95, 11), bytes.fromhex('41 8a 45 28 41 88 86 88 00 00 00'))
        code = read(0x9d6813, 7)
        self.assertEqual(0x9d681a + struct.unpack('<i', code[3:])[0], 0x2abfc40)
        self.assertEqual(struct.unpack('<Q', read(0x2545940, 8))[0], 0x9d67e0)
        self.assertEqual(struct.unpack('<Q', read(0x2545948, 8))[0], 0x273fb0)
        self.assertEqual(read(0x9b8d59, 9), bytes.fromhex('41 80 bc 24 8c 00 00 00 00'))
        self.assertEqual(read(0x9b8d6a, 8), bytes.fromhex('49 8b 84 24 90 00 00 00'))
        # Timer reads configured lap count from the same settings object as Team scoring.
        code = read(0x9b8d79, 7)
        self.assertEqual(0x9b8d80 + struct.unpack('<i', code[3:])[0], 0x2ad4e30)
        self.assertEqual(read(0x9b8d95, 6), bytes.fromhex('41 8b 47 0c 39 c3'))
        self.assertEqual(read(0x9b8dcb, 16), bytes.fromhex('41 8b 47 0c ff c8 48 98 48 8b 4b 20 44 8b 2c 81'))

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
    def test_finish_and_team_rows_bind_background_separately_from_name(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update2.pak'
        for name in ('octane_race_ranking', 'octane_battleteam_ranking'):
            entry = next(e for e in iga.pak_entries(pak) if e[0].endswith('/ui/' + name + '.igz'))
            with pak.open('rb') as f:
                g = Igz(iga.read_entry(f, *entry[1:]))
            behavior = next(o for o, t in g.objects() if t == 'CGuiBehaviorOctaneRaceRankingSelectItem')
            label, background = g.u64(behavior + 0xa8), g.u64(behavior + 0xc0)
            base = g.u64(background + 0x40)
            self.assertNotEqual(base, g.u64(label + 0x40))
            self.assertEqual(g.u64(base + 0x38), g.u64(background + 0x38))
            self.assertEqual(g.string_at(label + 0x28), '[PlayerTagText]')
            self.assertEqual(dict(g.objects())[base], 'igGuiPlaceable')

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
    def test_team_header_preserves_native_gradient_and_hides_extra_label(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update2.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower() == online_mode.TEAM_RANKING_UI)
        with pak.open('rb') as f:
            blob = iga.read_entry(f, *entry[1:])
        old, new = Igz(blob), Igz(online_mode.team_ranking_header(blob))
        self.assertEqual(old.data[0xaa0:0xbc0], new.data[0xaa0:0xbc0])
        # Like native RaceRanking, entrance animation must not undo DNF alpha.
        for tracks in (0x6268, 0x6650):
            self.assertEqual(new.u32(tracks + 12), 2)
            kept = [new.u64(new.u64(tracks + 32) + i * 8) for i in range(2)]
            self.assertTrue(all(dict(new.objects())[t] == 'igGuiVec3Track' for t in kept))
            self.assertEqual([new.u32(t + 0x18) for t in kept], [1, 2])
        marker = next(o for o, t in new.objects() if t == 'igGuiPlaceable' and
                      o + 0x28 in new.sites['RSTT'] and new.string_at(o + 0x28) == 'OCTR_TEAM_POINTS')
        self.assertTrue(new.u32(marker + 12) & 0x400)
        self.assertEqual(new.u64(marker + 0x38), old.u64(0xbc0 + 0x38))
        self.assertEqual(new.u32(0xa60 + 12), 3)
        avatar = new.u64(0x5a38 + 0x98)
        engine = new.u64(0x5a38 + 0xa0)
        self.assertEqual(new.string_at(engine + 0x28), 'OCTR_TEAM_ENGINE')
        for sprite in (avatar, engine):
            self.assertEqual(new.u32(sprite + 0xa0), 0)
            self.assertEqual(new.u32(sprite + 0xa4), 0)
        self.assertTrue(new.u32(engine + 12) & 0x400)
        self.assertEqual(struct.unpack_from('<2f', new.data, engine + 0x90), (80., 80.))
        self.assertEqual(new.string_at(0xe30 + 0x28), 'OCTR_TEAM_LIST')
        self.assertEqual(new.u32(0x73c0 + 0x90), old.u32(0x73c0 + 0x90))
        self.assertEqual(new.u32(0x53f0 + 0x94), old.u32(0x53f0 + 0x94))
        self.assertEqual(new.data[0x6e70 + 0x68], 1)  # Native name font shrinks to its box.
        self.assertEqual(new.data[0x6e70 + 0x69], 0)  # Single line; no spill into next row.
        self.assertEqual(new.string_at(avatar + 0x28), 'OCTR_TEAM_AVATAR')
        self.assertTrue(new.u32(avatar + 12) & 0x400)
        self.assertEqual(new.u64(avatar + 0x40), 0x52d0)
        self.assertEqual(new.u64(avatar + 0x48), 0)
        self.assertEqual(new.u64(avatar + 0xe8), 0)
        self.assertEqual(new.u64(avatar + 0xf0), 0)
        self.assertIn(avatar + 0x38, new.read_list('RHND', unpack_offsets))
        self.assertIn(0x5a38 + 0x98, new.sites['ROFS'])
        table = new.u64(0x52d0 + 0x48)
        self.assertEqual(new.u32(table + 12), old.u32(table + 12) + 2)
        self.assertEqual(sum(new.u64(new.u64(table + 32) + i * 8) == avatar
                             for i in range(new.u32(table + 12))), 1)
        def chain(g):
            result, at = [], 0x5b50
            while at and at not in result:
                result.append(at)
                at = g.u64(at + 0x58)
            return result
        self.assertEqual([o for o in chain(new) if o not in (avatar, engine)], chain(old))
        self.assertEqual(new.u64(0x5b50 + 0x58), avatar)
        self.assertEqual(new.u64(avatar + 0x50), 0x5b50)
        self.assertEqual(new.u64(avatar + 0x58), engine)
        self.assertEqual(new.u64(engine + 0x50), avatar)
        self.assertEqual(new.u64(engine + 0x58), 0x5c70)
        self.assertEqual(new.u64(0x5c70 + 0x50), engine)
        # Reuse a local export; no new sprite objects or external materials.
        for tag in ('EXID', 'TDEP'):
            self.assertEqual(new.fixup(tag)[3], old.fixup(tag)[3])
        self.assertEqual(new.u32(12), old.u32(12))
        self.assertEqual(sum(t == 'igGuiSprite' for _, t in new.objects()),
                         sum(t == 'igGuiSprite' for _, t in old.objects()))
        names = new.fixup('EXNM')
        handles = [(new.strings[a], new.strings[b & 0x7fffffff])
                   for a, b in struct.iter_unpack('<II', names[3][:names[1] * 8]) if b & 0x80000000]
        self.assertEqual(handles[new.u64(avatar + 0x38) & 0x7fffffff],
                         ('Portrait001', 'Octane_BattleTeam_Ranking'))
        self.assertEqual(handles[new.u64(engine + 0x38) & 0x7fffffff],
                         ('Portrait002', 'Octane_BattleTeam_Ranking'))
        self.assertNotIn('OCTR_TEAM_PORTRAIT_BORDER', new.strings)

    def test_unknown_graph_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'supported USA'):
            online_mode.rebuild(b'not a supported menu')

    def test_unknown_private_team_settings_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'unsupported native private team settings'):
            online_mode.private_team_settings(b'unknown settings')

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local USA/EU 1.21 game dump')
    def test_prepare_preserves_full_widescreen_layout(self):
        import archive_patch
        import widescreen
        from prepare import sfo
        game = Path(os.environ['BB_TEST_GAME'])
        info = sfo((game / 'sce_sys/param.sfo').read_bytes())
        elf = parse_self((game / 'eboot.bin').read_bytes())[0]
        pak = game / 'archives/update2.pak'
        entries = iga.pak_entries(pak)
        settings_index = next(i for i, entry in enumerate(entries)
                              if entry[0].lower() == online_mode.SETTINGS_ENTRY)
        lobby_index = next(i for i, entry in enumerate(entries)
                           if entry[0].lower() == online_mode.LOBBY_ENTRY)
        public_index = next(i for i, entry in enumerate(entries)
                            if entry[0].lower() == online_mode.PUBLIC_LOBBY_ENTRY)
        # The lobby-only regression misses other menus replaced by the layout.
        for ratio, output in (('21x9', '3440x1440'), ('21x9', '2560x1080'),
                              ('32x9', '5120x1440')):
            with self.subTest(ratio=ratio, output=output), tempfile.TemporaryDirectory() as temp:
                out = Path(temp)
                boot = out / 'eboot.elf'
                boot.write_bytes(elf)
                config = out / 'bbport.ini'
                config.write_text(f'aspect_ratio={ratio}\naspect_output={output}\nrender_res=1440\n')
                layout = widescreen.prepare(game, out, config, {}, info['TITLE_ID'],
                                             info['APP_VER'], {})
                self.assertIsNotNone(layout)
                base = layout['archive_patches']['/app0/archives/update2.pak']
                wide = online_mode.current_entry(pak, entries, settings_index, base)
                patches, _ = online_mode.prepare(game, out / 'online', boot,
                                                list(layout['archive_patches'].values()))
                result = next(p for p in patches
                              if archive_patch.read(p)[1] == '/app0/archives/update2.pak')
                actual = online_mode.current_entry(pak, entries, settings_index, result)
                self.assertEqual(actual, online_mode.private_team_settings(wide))
                old, new = Igz(wide), Igz(actual)
                allowed = set(range(0x51f8, 0x51fc)) | set(range(0xe070, 0xe07c))
                self.assertEqual(old.objects(), new.objects())
                self.assertEqual(old.fixups, new.fixups)
                self.assertEqual(len(old.data), len(new.data))
                self.assertTrue(all(a == b or i in allowed
                                    for i, (a, b) in enumerate(zip(old.data, new.data))))
                self.assertNotEqual(actual, online_mode.private_team_settings(
                    online_mode.current_entry(pak, entries, settings_index, None)))
                lobby = online_mode.current_entry(pak, entries, lobby_index, base)
                self.assertEqual(online_mode.current_entry(pak, entries, lobby_index, result),
                                 online_mode.lobby_card(lobby))
                public = online_mode.current_entry(pak, entries, public_index, base)
                actual_public = online_mode.current_entry(pak, entries, public_index, result)
                self.assertEqual(actual_public, online_mode.public_lobby_card(public))
                self.assertNotEqual(actual_public, online_mode.public_lobby_card(
                    online_mode.current_entry(pak, entries, public_index, None)))
                # Unknown edits still fail closed, even on a supported widescreen menu.
                with self.assertRaisesRegex(ValueError, 'unsupported native private team settings'):
                    online_mode.private_team_settings(wide + b'unknown edit')
                with self.assertRaisesRegex(ValueError, 'unsupported native public lobby card'):
                    online_mode.public_lobby_card(public + b'unknown edit')

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local USA/EU 1.21 game dump')
    def test_private_teams_visible_for_races_without_changing_native_selector(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update2.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower() == online_mode.SETTINGS_ENTRY)
        with pak.open('rb') as f:
            blob = iga.read_entry(f, *entry[1:])
        old, new = Igz(blob), Igz(online_mode.private_team_settings(blob))
        self.assertEqual(old.objects(), new.objects())
        self.assertEqual(old.fixups, new.fixups)
        selector = next(o for o, t in new.objects() if t == 'CGuiBehaviorTeamCountSettingSelector')
        size = new.sizes[new.u64(selector)]
        self.assertEqual(old.data[selector:selector + size], new.data[selector:selector + size])

        def values(row, tag):
            table = new.u64(row + 0xf0)
            for i in range(new.u32(table + 0x30)):
                animation = new.u64(new.u64(table + 0x18) + i * 8)
                if new.strings[new.u32(new.u64(animation + 0x10) + 0x10)] != tag:
                    continue
                tracks = new.u64(animation + 0x18)
                track = new.u64(new.u64(tracks + 0x20))
                keys = new.u64(track + 0x10)
                return new.u64(new.u64(keys + 0x20)) + 0x18
            self.fail(f'missing animation {tag}')

        row = 0x3910
        alpha = values(row, 'SingleRace')
        position = values(new.u64(row + 0x40), 'SingleRace')
        self.assertEqual(struct.unpack_from('<f', new.data, alpha), (1.0,))
        self.assertEqual(struct.unpack_from('<3f', new.data, position), (65.0, 180.0, 0.0))
        # Only the race visibility and spacing change; battle rules and navigation stay native.
        allowed = set(range(alpha, alpha + 4)) | set(range(position, position + 12))
        self.assertTrue(all(a == b or i in allowed for i, (a, b) in enumerate(zip(old.data, new.data))))

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
    def test_racer_receiver_hook_site(self):
        # src/runtime_online_mode.c hooks the peer customization receiver at 0xec03c0 - shift
        # (the selector hook's shift): its copied 17-byte prologue, and the function that logs
        # "RECEIVE PEER CUSTO UPDATE"; the game's racers list for mod racers comes along.
        from patches import load_segments
        game = Path(os.environ['BB_TEST_GAME'])
        elf = parse_self((game / 'eboot.bin').read_bytes())[0]
        shift = 0x1277e00 - online_mode.hook_site(elf)
        v, off, size = load_segments(elf)[0]
        at = off + 0xec03c0 - shift - v
        self.assertEqual(elf[at:at + 17].hex(), '554889e54157415641554154534883ec68')
        text = elf.index(b'**BRIBRI: RECEIVE PEER CUSTO UPDATE!')
        string = next(sv + text - so for sv, so, ss in load_segments(elf) if so <= text < so + ss)
        body = elf[at:at + 0x200]
        lea = body.index(bytes.fromhex('488d3d'), 0x100)
        self.assertEqual(0xec03c0 - shift + lea + 7 + struct.unpack_from('<i', body, lea + 3)[0], string)
        heroes = online_mode.known_heroes(game).split(',')
        self.assertIn('DriverSmallNorm', heroes)
        self.assertNotIn('DriverSonic', heroes)

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

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires native game dump')
    def test_team_panels_preloaded_with_dependency_closure(self):
        game = Path(os.environ['BB_TEST_GAME'])
        pak = game / 'archives/update2.pak'
        entries = {e[0].lower(): e for e in iga.pak_entries(pak)}
        with pak.open('rb') as f:
            blob = iga.read_entry(f, *entries[online_mode.ONLINE_PACKAGE][1:])
            source = iga.read_entry(f, *entries[online_mode.START_PACKAGE][1:])
            race = iga.read_entry(f, *next(e for name, e in entries.items()
                                        if name.endswith('/juicedomain_race_pkg.igz'))[1:])
        with tempfile.TemporaryDirectory() as temp:
            assets = Path(temp)
            new = Igz(online_mode.cup_package(game, blob, source, assets, race))
            _, rows = online_mode.package_rows(new)
            self.assertEqual(len(rows), len(set(rows)))
            for name in ('octane_race_ranking', 'octane_battleteam_ranking', 'octane_race_leaders'):
                self.assertIn(('gui_project', 'ui/' + name + '.igz'), rows)
            self.assertTrue(any('race' in path.as_posix() for path in assets.rglob('*') if path.is_file()))
            self.assertTrue(any('race_ranking_materials' in path for _, path in rows))
            self.assertTrue(any('battleteam_ranking_materials' in path for _, path in rows))
            self.assertTrue(any('racerportrait_contour' in path for _, path in rows))

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires native game dump')
    def test_team_selector_marks_only_post_open_disable(self):
        for archive in ('update', 'update2'):
            pak = Path(os.environ['BB_TEST_GAME']) / 'archives' / (archive + '.pak')
            entry = next(e for e in iga.pak_entries(pak)
                         if e[0].lower().endswith('/octane_lobby_privategame_root.igz'))
            with pak.open('rb') as f:
                blob = iga.read_entry(f, *entry[1:])
            old, new = Igz(blob), Igz(online_mode.team_selector_callback(blob))
            changed = [o for o, t in old.objects()
                       if old.data[o:o + old.sizes[old.u64(o)]] != new.data[o:o + old.sizes[old.u64(o)]]]
            self.assertEqual(len(changed), 1)
            node = changed[0]
            self.assertEqual(dict(old.objects())[node], 'igVscCppActionNode')
            self.assertEqual(new.u32(node + 16), 3)
            self.assertEqual(new.u64(new.u64(node + 32) + 16), 0)
            self.assertEqual(old.data[old.u64(node + 32):old.u64(node + 32) + 16],
                             new.data[new.u64(node + 32):new.u64(node + 32) + 16])
            self.assertEqual(old.data[node + 40:node + 64], new.data[node + 40:node + 64])

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
        self.assertEqual(at(0xf1fb50 - shift, 17), bytes.fromhex('554889e54157415641554154534883ec38'))
        self.assertEqual(at(0xe330e0, 17), bytes.fromhex('554889e54157415641554154534883ec48'))
        self.assertEqual(at(0xeb7950 - shift, 17), bytes.fromhex('554889e54157415653504889fb488b4318'))
        self.assertEqual(at(0xeb71d0 - shift, 17), bytes.fromhex('554889e54157415641554154534883ec58'))
        self.assertEqual(at(0x1093ca0 - shift, 17), bytes.fromhex('554889e54157415641554154534883ec38'))
        confetti = bytearray.fromhex('554889e54157415653508a0568a5b501')
        confetti[12] += shift
        self.assertEqual(at(0xf382f0 - shift, 16), confetti)

        network_branch = bytearray.fromhex('554889e54156534889d34989fee8be6393ff')
        network_branch[14] += shift
        self.assertEqual(at(0x115a740 - shift, 18), network_branch)
        self.assertEqual(struct.unpack('<Q', at(0x2778150, 8))[0], 0x1293030 - shift)
        self.assertEqual(at(0x253970, 18), bytes.fromhex("8b474831d28d48ff83f8010f4fd1895748c3"))
        self.assertEqual(at(0x253990, 4), bytes.fromhex("ff4748c3"))
        self.assertEqual(at(0xa09190, 12), bytes.fromhex("554889e553504889fb4885db"))
        for slot, target in ((0x2543d68, 0x9c1250), (0x2545940, 0x9d67e0), (0x25436c8, 0x9b73f0), (0x2546008, 0x9de1b0), (0x2546018, 0x9dedb0)):
            self.assertEqual(struct.unpack('<Q', at(slot, 8))[0], target)
        for address in (0xe9de80, 0xe9dcf0):
            self.assertEqual(at(address - shift, 17),
                             bytes.fromhex('55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18'))
        self.assertEqual(at(0xff1810 - shift, 16),
                         bytes.fromhex('55 48 89 e5 41 57 41 56 53 50 49 89 ff 49 8b 47'))
        self.assertEqual(at(0xc2ed20, 11), bytes.fromhex('55 48 89 e5 41 56 53 48 83 ec 10'))
        self.assertEqual(at(0x9c153f, 19), bytes.fromhex('e8') +
                         struct.pack('<i', 0xae4cfc - shift) +
                         bytes.fromhex('48 8b 8d 68 ff ff ff 4c 8b bd 40 ff ff ff'))
        self.assertEqual(at(0xeb79d3 - shift, 21),
                         bytes.fromhex('48 8b 05') + struct.pack('<i', 0x1c1646e + shift) +
                         bytes.fromhex('8b 80 40 0d 00 00 83 c0 f7 83 f8 04 77 ae'))
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
        # Native team movement checks the raw next index before wrapping it.
        # When every team is full this can miss the starting team indefinitely.
        self.assertEqual(at(0x9c493c, 15),
                         bytes.fromhex('44 39 d8 7c 0f 44 01 c9 44 39 d1 89 ce 75 b5'))
        self.assertEqual(at(0xff2320 - shift, 8), bytes.fromhex('55 48 89 e5 41 57 41 56'))
        self.assertEqual(at(0xff11a0 - shift, 14),
                         bytes.fromhex('55 48 89 e5 41 57 41 56 41 55 41 54 53 50'))
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
        self.assertEqual(card_new.objects()[:len(card_old.objects())], card_old.objects())
        self.assertEqual(card_new.data[:0x200], card_old.data[:0x200])
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
        self.assertEqual(card_new.objects(), card_old.objects())
        self.assertNotIn('OCTR_PRIVATE_TEAM_BUTTON', card_new.strings)
        old, new = Igz(original), Igz(online_mode.rebuild(original))
        # The native traversal and child array must agree. A second selector clone
        # previously displaced RACE TYPE from the array but left it in the chain.
        children = [new.u64(new.u64(0xfe0) + i * 8) for i in range(new.u32(0xfcc))]
        for previous, following in zip(children, children[1:]):
            self.assertEqual(new.u64(previous + 0x58), following)
            self.assertEqual(new.u64(following + 0x50), previous)
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
