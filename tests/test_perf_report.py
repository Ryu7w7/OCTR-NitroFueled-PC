"""The anonymous performance report (scripts/perf_report.py)."""
from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import json
import tempfile
import unittest
from pathlib import Path

import perf_report


def perf_line(frames, ms, worst, long_frames, hist):
    return f'Perf: frames {frames} ms {ms:.1f} worst {worst:.1f} long {long_frames} h {",".join(map(str, hist))}\n'


class PerfReportTests(unittest.TestCase):
    def collector(self):
        c = perf_report.Collector()
        c.feed('GPU: NVIDIA GeForce GTX 1650, 3935 MiB VRAM; NVIDIA 610.88.0.0 (610.88), Vulkan 1.4.341\n')
        c.feed("LOADING LEVEL: 'Octane/Octane_StartScreen/Octane_StartScreen'\n")
        menus = [0] * 14
        menus[6] = 300  # 300 frames of 14-16 ms
        c.feed(perf_line(300, 4500.0, 15.9, 0, menus))
        c.feed("LOADING LEVEL: 'octane/t131_coco_park/t131_coco_park'\n")
        c.feed('------------- level finished loading in 10.62 secs (41.90)-------------\n')
        race = [0] * 14
        race[3] = 590   # 8.5-10 ms
        race[10] = 8    # 25-33.4 ms
        race[11] = 2    # 33.4-50 ms: slow
        c.feed(perf_line(600, 5600.0, 47.0, 3, race))
        c.feed('ONLINE nickname SomePlayer joined from 203.0.113.9\n')  # never reported
        return c

    def test_tracks_and_stats(self):
        report = self.collector().report({'renderer': 'shadps4'})
        self.assertEqual(report['gpu']['model'], 'NVIDIA GeForce GTX 1650')
        self.assertEqual(report['gpu']['vram_mib'], 3935)
        self.assertEqual(set(report['tracks']), {'menus', 't131_coco_park'})
        coco = report['tracks']['t131_coco_park']
        self.assertAlmostEqual(coco['avg_fps'], 107.1, places=1)
        self.assertEqual(coco['low_1pct_fps'], round(1000 / 33.4, 1))  # the 1% slowest reach 25-33 ms
        self.assertEqual(coco['slow_frames_pct'], round(100 * 2 / 600, 2))
        self.assertEqual(coco['loading_frames'], 3)
        self.assertEqual(coco['load_seconds'], [10.62])
        self.assertEqual(report['overall']['seconds'], 10.1)
        self.assertEqual(report['renderer'], 'shadps4')

    def test_nothing_personal(self):
        text = json.dumps(self.collector().report({}))
        for private in ('SomePlayer', '203.0.113.9', 'nickname'):
            self.assertNotIn(private, text)

    def test_settings_whitelist_and_consent(self):
        with tempfile.TemporaryDirectory() as d:
            ini = Path(d) / 'bbport.ini'
            ini.write_text('render_res=1440\nonline_name=SomePlayer\nupscaler=fsr1\nperf_reports=1\n',
                           encoding='utf-8')
            self.assertEqual(perf_report.settings_of(ini), {'render_res': '1440', 'upscaler': 'fsr1'})
            self.assertEqual(perf_report.consent(ini), '1')
            ini.write_text('render_res=1080\n', encoding='utf-8')
            self.assertEqual(perf_report.consent(ini), '')  # not asked yet: nothing is sent

    def test_keep_only_the_last(self):
        with tempfile.TemporaryDirectory() as d:
            for i in range(perf_report.KEEP + 3):
                (Path(d) / f'2026010{i:02d}_000000.json').write_text('{}', encoding='utf-8')
            perf_report.keep({'x': 1}, d)
            self.assertEqual(len(list(Path(d).glob('*.json'))), perf_report.KEEP)

    def test_tee_and_finish(self):
        """run.sh's path: every line passed on unchanged, then the report kept (no consent: not sent)."""
        import io
        lines = ("GPU: AMD Radeon RX 6600 (RADV NAVI23), 8176 MiB VRAM; radv Mesa 25.1, Vulkan 1.4.311\n"
                 "LOADING LEVEL: 'octane/t111_crash_cove/t111_crash_cove'\n"
                 "------------- level finished loading in 4.80 secs (41.90)-------------\n"
                 + perf_line(300, 5000.0, 20.0, 0, [0] * 7 + [300] + [0] * 6))
        with tempfile.TemporaryDirectory() as d:
            sink = io.BytesIO()
            state = Path(d) / 'state.json'
            perf_report.tee(state, io.BytesIO(lines.encode()), sink)
            self.assertEqual(sink.getvalue().decode(), lines)
            ini = Path(d) / 'bbport.ini'
            ini.write_text('render_res=1080\n', encoding='utf-8')
            perf_report.finish(state, 0, ini, d, 'v0.7-test', 'CUSA13795', say=lambda _t: None)
            kept = list((Path(d) / 'perf').glob('*.json'))
            self.assertEqual(len(kept), 1)
            report = json.loads(kept[0].read_text(encoding='utf-8'))
            self.assertEqual(report['tracks']['t111_crash_cove']['load_seconds'], [4.8])
            self.assertEqual(report['gpu']['vram_mib'], 8176)
            self.assertEqual(report['exit']['meaning'], 'normal')

    def test_levels(self):
        self.assertEqual(perf_report.short_level('octane/t231_out_of_time/t231_out_of_time'), 't231_out_of_time')
        self.assertEqual(perf_report.short_level('octane/octane_startscreen/octane_startscreen'), 'menus')

    def test_crash_signature_without_personal_data(self):
        # probe.c's fault lines: the kind, what was touched and where; no address, path, IP or thread
        c = perf_report.Collector()
        c.feed("LOADING LEVEL: 'octane/t111_crash_cove/t111_crash_cove'\n"
               "Fault: read access to 0000000000000010 at guest offset 0x12ab34 (thread 4321)\n"
               "Fault: write access to 0000000000000020 at guest offset 0x99 (thread 1)\n")
        report = c.report({})
        self.assertEqual(report['ended_in'], 't111_crash_cove')
        self.assertEqual(report['fault'], {'kind': 'read access', 'target': 'null+0x10',
                                           'where': 'guest offset 0x12ab34'})
        abort = perf_report.fault_of('Fault: abort() called (assert: C:\\Users\\Some Player\\a.cpp:3 '
                                     'via 203.0.113.9) at ucrtbase.dll+0x1234 (thread 7)')
        self.assertEqual(abort['where'], 'ucrtbase.dll+0x1234')
        for private in ('Users', 'Some Player', '203.0.113.9'):
            self.assertNotIn(private, abort['reason'])
        self.assertEqual(perf_report.fault_of('Fault: exception 0x20474343 at 00007ffabcdef012 (thread 1)'),
                         {'kind': 'exception 0x20474343', 'where': 'unknown'})
        self.assertIsNone(perf_report.fault_of('Graphics engine: native Vulkan (experimental)'))
        self.assertNotIn('fault', self.collector().report({}))

    def test_a_lost_gpu_device_says_why_without_addresses(self):
        # vk_breadcrumbs.cpp's report: where it was noticed, the driver's words, the bad access,
        # a buffer freed under the GPU, and the command it stopped in (hashes kept, addresses not)
        c = perf_report.Collector()
        c.feed("LOADING LEVEL: 'octane/t112_roos_tubes/t112_roos_tubes'\n"
               'GPU fault info: VK_EXT_device_fault on, a device loss reports its cause\n'
               'GPU breadcrumbs at device lost (submit): what each command stream finished\n'
               '  stream 0 (gfx): 812 commands noted, the GPU started #700 and did not finish it\n'
               '    #699 finished: Pass main\n'
               '    #700 STUCK: DrawIndirect vs 00112233aabbccdd ps 44556677eeff0011, up to 4 draws, '
               'stride 20, arguments at 0x10f6901f78 (guest memory now 1 2 3 4 5)\n'
               'GPU fault info: Page fault (1 addresses, 1 vendor records)\n'
               '  invalid read at GPU address 0x1234567000 (range 0x1234567000-0x1234567fff)\n'
               '    inside buffer 0x1f2e3d (GPU 0x1234560000, 65536 bytes) destroyed 3.1 ms before this report\n')
        fault = c.report({})['fault']
        self.assertEqual(fault['kind'], 'gpu device lost')
        self.assertEqual(fault['where'], 'submit')
        self.assertEqual(fault['cause'], 'Page fault')
        self.assertEqual(fault['gpu_access'], 'invalid read')
        self.assertEqual(fault['freed'], 'buffer')
        self.assertIn('vs 00112233aabbccdd ps 44556677eeff0011', fault['stuck'])
        self.assertNotIn('0x10f6901f78', fault['stuck'])
        self.assertNotIn('guest memory', fault['stuck'])
        # the status line printed at startup is not a loss
        quiet = perf_report.Collector()
        quiet.feed('GPU fault info: VK_EXT_device_fault on, a device loss reports its cause\n')
        self.assertNotIn('fault', quiet.report({}))

    def test_a_corrupt_free_the_game_survived_is_reported(self):
        c = perf_report.Collector()
        c.feed('Runtime: bailing corrupt free at guest offset 0x1590e5d (block 000000106d171ef8, '
               'r8=000000106d171ee8, pool=000000106cf73e10)\n')
        self.assertTrue(c.corrupt_free)
        self.assertEqual(c.fault, {'kind': 'corrupt free', 'where': 'guest offset 0x1590e5d'})

    def test_a_crash_after_a_corrupt_free_is_the_one_reported(self):
        c = perf_report.Collector()
        c.feed('Runtime: bailing corrupt free at guest offset 0x1590e5d (block 1, r8=2, pool=3)\n')
        c.feed('Fault: read access to 0000000000000010 at guest offset 0x123456 (thread 7)\n')
        self.assertTrue(c.corrupt_free)
        self.assertEqual(c.fault['kind'], 'read access')

    def test_a_corrupt_block_says_what_was_around_it(self):
        # probe.c heap_summary: word kinds, never data, kept even when a crash follows
        c = perf_report.Collector()
        c.feed('Runtime: bailing corrupt free at guest offset 0x1590e5d (block 1, r8=2, pool=3)\n'
               'Runtime: corrupt block words (block-0x40..+0x18): 0 n41 b-30 img+2774cc0 txt ptr ? '
               'Jose b+10 0 unread unread; callers: 0x14a290d 0x1518e5a\n')
        heap = c.report({})['fault']['heap']
        self.assertEqual(heap['words'][:4], ['0', 'n41', 'b-30', 'img+2774cc0'])
        self.assertEqual(heap['words'][7], '?')  # not a kind: dropped
        self.assertEqual(heap['callers'], ['0x14a290d', '0x1518e5a'])
        c.feed('Fault: write access to 0000000000000020 at guest offset 0x1490000 (thread 7)\n')
        self.assertEqual(c.report({})['fault']['heap'], heap)

    def test_a_crash_names_its_callers_and_thread(self):
        c = perf_report.Collector()
        c.feed('Fault: write access to 0000000000000020 at guest offset 0x1490000 (thread 7)\n'
               '  rax=0000000000000001 rbx=0000000000000002\n'
               '  guest code addresses on the stack (callers, newest first): 0x14a290d@rsp+0x8 '
               '0x1518e5a@rsp+0x40\n'
               '  thread name: C:\\Users\\Some Player\n'
               '  guest thread: MainThread\n'
               'Fault: read access to 0000000000000010 at guest offset 0x99 (thread 1)\n'
               '  guest code addresses on the stack (callers, newest first): 0x1@rsp+0x8\n'
               '  guest thread: Other\n')
        fault = c.report({})['fault']
        self.assertEqual(fault['where'], 'guest offset 0x1490000')
        self.assertEqual(fault['callers'], ['0x14a290d', '0x1518e5a'])
        self.assertEqual(fault['thread'], 'MainThread')
        self.assertNotIn('Some Player', json.dumps(fault))

    def test_mods_in_use(self):
        with tempfile.TemporaryDirectory() as folder:
            mods = Path(folder) / 'mods'
            for name in ('B kart', 'A skin', 'Off'):
                (mods / name / 'dvdroot_ps4').mkdir(parents=True)
            (Path(folder) / 'mods.json').write_text(json.dumps({'disabled': ['Off']}))
            self.assertEqual(perf_report.mods_in_use(folder), ['A skin', 'B kart'])
            self.assertEqual(perf_report.mods_in_use(Path(folder) / 'nothing'), [])
            # Mods page packages: named by mod_packages.py, while loose mods are off.
            env = {'BB_MOD_PACKAGES_USED': 'Gwen,Miku racer', 'BB_MODS_ENABLED': '0'}
            self.assertEqual(perf_report.mods_in_use(folder, env), ['Gwen', 'Miku racer'])
            env['BB_MODS_ENABLED'] = '1'
            self.assertEqual(perf_report.mods_in_use(folder, env), ['Gwen', 'Miku racer', 'A skin', 'B kart'])
            self.assertEqual(perf_report.mods_in_use(folder, {'BB_MODS_ENABLED': '0'}), [])


if __name__ == '__main__':
    unittest.main()
