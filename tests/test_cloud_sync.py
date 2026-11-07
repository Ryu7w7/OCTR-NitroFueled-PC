import io
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import cloud_sync  # noqa: E402


class FakeServer:
    """The server's two routes as cloud_sync calls them: merges trophies, keeps saves."""

    def __init__(self, unlocked=''):
        self.unlocked = cloud_sync.parse_unlocks(unlocked)
        self.saves = []

    def __call__(self, method, target, body=None, timeout=10):
        if target.startswith('/ryunet/trophies/sync'):
            for tid, at in cloud_sync.parse_unlocks(body.decode()).items():
                self.unlocked[tid] = min(at, self.unlocked.get(tid, at))
            return {'unlocked': ''.join(f'{t} {a}\n' for t, a in sorted(self.unlocked.items()))}
        if target.startswith('/ryunet/saves/upload'):
            self.saves.append(body)
            return {'id': len(self.saves)}
        raise AssertionError(target)


class CloudSyncTests(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.say = []

    def test_trophies_from_another_pc_are_added_never_removed(self):
        path = cloud_sync.trophy_file(self.dir, 'CUSA13795')
        path.parent.mkdir(parents=True)
        path.write_text('1 1792000000\n2 1792000500\n')
        server = FakeServer('2 1792000100\n3 1792000200\n')
        self.assertEqual(cloud_sync.sync_trophies(server, 'Neki', self.dir, 'CUSA13795', say=self.say.append), 3)
        local = cloud_sync.parse_unlocks(path.read_text())
        self.assertEqual(sorted(local), [1, 2, 3])
        self.assertEqual(local[3], 1792000200)
        # The server learned this PC's 1 too, and a second sync adds nothing.
        self.assertIn(1, server.unlocked)
        before = path.read_text()
        cloud_sync.sync_trophies(server, 'Neki', self.dir, 'CUSA13795', say=self.say.append)
        self.assertEqual(path.read_text(), before)

    def make_save(self, text):
        base = cloud_sync.save_dir(self.dir, 'CUSA13795')
        (base / 'savedata0').mkdir(parents=True, exist_ok=True)
        (base / 'savedata0' / 'GlobalSave.dat').write_text(text)
        (base / 'savedata0.sce_sys').mkdir(exist_ok=True)
        (base / 'savedata0.sce_sys' / 'param.sfo').write_text('sfo')
        (base / 'RaceReplay').mkdir(exist_ok=True)
        (base / 'RaceReplay' / 'A00001.crr').write_text('replay')

    def test_save_uploaded_only_when_it_changed_and_without_replays(self):
        server = FakeServer()
        self.assertIsNone(cloud_sync.upload_save(server, 'Neki', self.dir, 'CUSA13795', say=self.say.append))  # no save yet
        self.make_save('one')
        self.assertEqual(cloud_sync.upload_save(server, 'Neki', self.dir, 'CUSA13795', say=self.say.append), 1)
        self.assertIsNone(cloud_sync.upload_save(server, 'Neki', self.dir, 'CUSA13795', say=self.say.append))
        names = zipfile.ZipFile(io.BytesIO(server.saves[0])).namelist()
        self.assertIn('savedata0/GlobalSave.dat', names)
        self.assertFalse(any(n.startswith('RaceReplay') for n in names))
        self.make_save('two')
        self.assertEqual(cloud_sync.upload_save(server, 'Neki', self.dir, 'CUSA13795', say=self.say.append), 2)

    def test_restore_keeps_a_backup_and_refuses_paths_outside(self):
        self.make_save('mine')
        cloud = cloud_sync.zip_save(self.dir, 'CUSA13795')
        self.make_save('changed here')
        cloud_sync.restore_save(cloud, self.dir, 'CUSA13795')
        base = cloud_sync.save_dir(self.dir, 'CUSA13795')
        self.assertEqual((base / 'savedata0' / 'GlobalSave.dat').read_text(), 'mine')
        self.assertTrue((self.dir / 'cloud' / 'backup-CUSA13795.zip').exists())
        self.assertTrue((base / 'RaceReplay' / 'A00001.crr').exists())  # replays untouched
        bad = io.BytesIO()
        with zipfile.ZipFile(bad, 'w') as zf:
            zf.writestr('savedata0/../../evil.txt', 'x')
        with self.assertRaises(ValueError):
            cloud_sync.restore_save(bad.getvalue(), self.dir, 'CUSA13795')
        self.assertFalse((self.dir / 'evil.txt').exists())


if __name__ == '__main__':
    unittest.main()
