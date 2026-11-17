"""tools/install_from_pkg.sh on fake packages: no game file is involved.

The packages are tiny files with a PS4 .pkg header and a Content ID; the extractor is a shell
script that makes the folders the real one makes (CUSA13795 for a base game, CUSA13795-patch for
an update). tools/merge_update.py is the real one; scripts/game_check.py is replaced by a stub
(the real one needs a real eboot.bin).
"""
from paths import ROOT
import http.server
import os
import shutil
import stat
import subprocess
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path

SCRIPT = ROOT / 'tools/install_from_pkg.sh'
CID = 'UP0002-CUSA13795_00-CRASHTEAMRACING1'
EU_CID = 'EP0002-CUSA14876_00-CRASHTEAMRACING1'

FAKE_EXTRACTOR = r'''#!/usr/bin/env bash
echo "$1" >> "$(dirname "$0")/extractor.log"
title=$(grep -aoE 'CUSA[0-9]{5}' "$1" | head -1)
if grep -aq A0121 "$1"; then d="$2/$title-patch"; ver=01.21; mkdir -p "$d/sce_sys"
else d="$2/$title"; ver=01.00; mkdir -p "$d/sce_sys" "$d/sce_module"; echo mod > "$d/sce_module/libc.prx"; fi
echo "eboot $ver" > "$d/eboot.bin"
echo "param $ver" > "$d/sce_sys/param.sfo"
echo "THE END"
'''

STUB_CHECK = '''import sys
from pathlib import Path
game = Path(sys.argv[1])
if 'eboot 01.21' not in (game / 'eboot.bin').read_text():
    sys.exit('Unsupported game files: the update is not merged')
print('ctr')
'''


def pkg(path, cid, marker=b'BASE'):
    """A fake .pkg: the PS4 magic, the Content ID at offset 0x40, then a marker."""
    path.write_bytes(b'\x7fCNT' + bytes(60) + cid.encode().ljust(40, b'\0') + marker)
    return path


class InstallFromPkg(unittest.TestCase):
    def setUp(self):
        if shutil.which('bash') is None:
            self.skipTest('bash is needed')
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        self.base = pkg(self.tmp / 'base.pkg', CID)
        self.update = pkg(self.tmp / 'update.pkg', CID, b'A0121')
        self.out = self.tmp / 'out'
        (self.tmp / 'bin').mkdir()
        self.extractor = self.tmp / 'bin/pkg_extractor.AppImage'
        self.extractor.write_text(FAKE_EXTRACTOR)
        self.extractor.chmod(self.extractor.stat().st_mode | stat.S_IXUSR)
        self.check = self.tmp / 'game_check.py'
        self.check.write_text(STUB_CHECK)
        self.env = dict(os.environ, INSTALL_MIN_PKG_MB='0', INSTALL_MIN_FREE_GB='0', GAME_CHECK=str(self.check),
                        PKG_EXTRACTOR='', PKG_SEARCH_DIRS=str(self.tmp / 'Downloads'), HOME=str(self.tmp),
                        NO_PROXY='127.0.0.1', no_proxy='127.0.0.1')

    def run_script(self, *args, env=None, stdin=subprocess.DEVNULL):
        return subprocess.run(['bash', str(SCRIPT), *args], env=env or self.env, capture_output=True,
                              text=True, stdin=stdin, cwd=self.tmp, timeout=120)

    def install(self, *extra):
        return self.run_script('--base', str(self.base), '--update', str(self.update), '--out', str(self.out),
                               '--extractor', str(self.extractor), *extra)

    def extracted(self):
        log = self.tmp / 'bin/extractor.log'
        return log.read_text().split() if log.exists() else []

    def test_installs_and_merges_with_hard_links(self):
        r = self.install()
        self.assertEqual(r.returncode, 0, r.stderr)
        merged = self.out / 'CUSA13795-1.21'
        self.assertEqual((merged / 'eboot.bin').read_text(), 'eboot 01.21\n')
        self.assertEqual((merged / 'sce_sys/param.sfo').read_text(), 'param 01.21\n')
        self.assertEqual((merged / 'sce_module/libc.prx').read_text(), 'mod\n')
        self.assertEqual(os.stat(merged / 'sce_module/libc.prx').st_nlink, 2)
        self.assertEqual((self.out / 'CUSA13795/eboot.bin').read_text(), 'eboot 01.00\n')
        self.assertIn(str(merged), r.stdout)
        self.assertEqual([p.name for p in self.out.iterdir() if p.name.startswith('.tmp')], [])

    def test_second_run_does_nothing(self):
        self.assertEqual(self.install().returncode, 0)
        r = self.install()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('Already installed', r.stdout)
        self.assertEqual(len(self.extracted()), 2)

    def test_reuses_extracted_folders(self):
        self.assertEqual(self.install().returncode, 0)
        shutil.rmtree(self.out / 'CUSA13795-1.21')
        self.assertEqual(self.install().returncode, 0)
        self.assertEqual(len(self.extracted()), 2)
        self.assertTrue((self.out / 'CUSA13795-1.21/eboot.bin').is_file())

    def test_copies_when_merge_tool_is_missing(self):
        env = dict(self.env, MERGE_UPDATE=str(self.tmp / 'missing.py'))
        r = self.run_script('--base', str(self.base), '--update', str(self.update), '--out', str(self.out),
                            '--extractor', str(self.extractor), env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        merged = self.out / 'CUSA13795-1.21'
        self.assertEqual((merged / 'eboot.bin').read_text(), 'eboot 01.21\n')
        self.assertEqual(os.stat(merged / 'sce_module/libc.prx').st_nlink, 1)

    def test_rejects_mixed_editions(self):
        eu = pkg(self.tmp / 'eu-update.pkg', EU_CID, b'A0121')
        r = self.run_script('--base', str(self.base), '--update', str(eu), '--out', str(self.out),
                            '--extractor', str(self.extractor))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('cannot be mixed', r.stderr)
        self.assertEqual(self.extracted(), [])

    def test_accepts_the_european_edition(self):
        base = pkg(self.tmp / 'eu-base.pkg', EU_CID)
        update = pkg(self.tmp / 'eu-update.pkg', EU_CID, b'A0121')
        r = self.run_script('--base', str(base), '--update', str(update), '--out', str(self.out),
                            '--extractor', str(self.extractor))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((self.out / 'CUSA14876-1.21/eboot.bin').is_file())
        self.assertIn('European edition', r.stderr)

    def test_rejects_other_games_and_bad_files(self):
        other = pkg(self.tmp / 'other.pkg', 'UP0001-CUSA00001_00-SOMETHINGELSE00')
        bad = self.tmp / 'bad.pkg'
        bad.write_text('not a package')
        for wrong in (other, bad, self.tmp / 'nothing.pkg'):
            r = self.run_script('--base', str(wrong), '--update', str(self.update), '--out', str(self.out),
                                '--extractor', str(self.extractor))
            self.assertNotEqual(r.returncode, 0, wrong)
        self.assertEqual(self.extracted(), [])

    def test_small_pkg_is_reported_as_incomplete(self):
        env = dict(self.env, INSTALL_MIN_PKG_MB='1000')
        r = self.run_script('--base', str(self.base), '--update', str(self.update), '--out', str(self.out),
                            '--extractor', str(self.extractor), env=env)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('incomplete', r.stderr)

    def test_swapped_base_and_update_are_detected(self):
        r = self.run_script('--base', str(self.update), '--update', str(self.base), '--out', str(self.out),
                            '--extractor', str(self.extractor))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('swap --base and --update', r.stderr)
        self.assertFalse((self.out / 'CUSA13795').exists())
        self.assertEqual([p.name for p in self.out.iterdir() if p.name.startswith('.tmp')], [])

    def test_failed_game_check_is_reported(self):
        self.check.write_text('import sys; sys.exit("Unsupported game files: test")')
        r = self.install()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('Unsupported game files: test', r.stderr)

    def test_needs_an_extractor(self):
        r = self.run_script('--base', str(self.base), '--update', str(self.update), '--out', str(self.out))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('--download-extractor', r.stderr)

    def test_finds_the_packages_by_itself(self):
        downloads = self.tmp / 'Downloads'
        downloads.mkdir()
        pkg(downloads / 'a.pkg', CID, b'BASE' + bytes(100))   # the bigger one is the base game
        pkg(downloads / 'b.pkg', CID, b'A0121')
        pkg(downloads / 'other-game.pkg', 'UP0001-CUSA00001_00-SOMETHINGELSE00', b'BASE' + bytes(500))  # ignored
        r = self.run_script('--out', str(self.out), '--extractor', str(self.extractor), '--yes')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.out / 'CUSA13795-1.21/eboot.bin').read_text(), 'eboot 01.21\n')

    def test_downloads_the_extractor_from_a_zip(self):
        srv = self.tmp / 'srv'
        (srv / 'dl').mkdir(parents=True)
        release = srv / 'repos/AzaharPlus/shadPS4Plus/releases/tags'
        release.mkdir(parents=True)
        with zipfile.ZipFile(srv / 'dl/extractor.zip', 'w') as z:
            z.write(self.extractor, 'pkg_extractor.AppImage')
        class Quiet(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **k):
                super().__init__(*a, directory=str(srv), **k)

            def log_message(self, *args):
                pass

        httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Quiet)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        self.addCleanup(httpd.server_close)
        self.addCleanup(httpd.shutdown)
        url = f'http://127.0.0.1:{httpd.server_address[1]}'
        (release / 'PKG_EXTRACTOR_1_0').write_text('{"assets":[{"browser_download_url": "%s/dl/extractor.zip"}]}' % url)
        env = dict(self.env, GITHUB_API=url)
        r = self.run_script('--base', str(self.base), '--update', str(self.update), '--out', str(self.out),
                            '--download-extractor', env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((self.out / 'pkg_extractor.AppImage').is_file())
        self.assertTrue((self.out / 'CUSA13795-1.21/eboot.bin').is_file())


if __name__ == '__main__':
    unittest.main()
