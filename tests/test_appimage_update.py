import hashlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "launcher"))
import appimage_update as au  # noqa: E402


class _Reply(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def release_json(tag="v0.5.5", data=b"new appimage", digest=True):
    return json.dumps({"tag_name": tag, "assets": [
        {"name": f"CTR-NitroFueled-PC-portable-{tag}-windows.zip", "browser_download_url": "https://x/w.zip", "size": 5},
        {"name": f"SacredFire-{tag}-x86_64.AppImage", "browser_download_url": "https://x/a.AppImage", "size": len(data),
         "digest": "sha256:" + hashlib.sha256(data).hexdigest() if digest else None},
    ]}).encode()


class AppImageUpdateTests(unittest.TestCase):
    def test_versions_follow_the_windows_rules(self):
        self.assertEqual(au.parse_version("v0.5.4"), (0, 5, 4, 1))
        self.assertEqual(au.parse_version("v0.5.4-linux"), (0, 5, 4, 1))
        self.assertEqual(au.parse_version("v0.5.4-3-gabc123-dirty"), (0, 5, 4, 2))
        self.assertEqual(au.parse_version("v0.5.5-rc1"), (0, 5, 5, 0))
        self.assertIsNone(au.parse_version("abc123"))
        self.assertTrue(au.is_newer(au.parse_version("v0.5.4-linux"), au.parse_version("v0.5.5")))
        self.assertTrue(au.is_newer(au.parse_version("v0.5.5-rc1"), au.parse_version("v0.5.5")))
        self.assertFalse(au.is_newer(au.parse_version("v0.5.5-2-gdeadbeef"), au.parse_version("v0.5.5")))
        self.assertFalse(au.is_newer(au.parse_version("v0.5.5"), au.parse_version("v0.5.5")))
        self.assertFalse(au.is_newer(None, au.parse_version("v0.5.5")))

    def test_release_picks_the_appimage(self):
        r = au.parse_release(release_json())
        self.assertEqual((r["tag"], r["name"], r["url"]), ("v0.5.5", "SacredFire-v0.5.5-x86_64.AppImage", "https://x/a.AppImage"))
        self.assertEqual(len(r["sha256"]), 64)
        self.assertIsNone(au.parse_release(json.dumps({"tag_name": "v0.5.5", "assets": []}).encode()))

    def test_install_replaces_the_old_appimage(self):
        data = b"new appimage"
        with tempfile.TemporaryDirectory() as d:
            old = Path(d) / "SacredFire-v0.5.4-x86_64.AppImage"
            old.write_bytes(b"old")
            seen = []
            new = au.install(au.parse_release(release_json(data=data)), old,
                             progress=lambda done, total: seen.append((done, total)),
                             opener=lambda req, timeout: _Reply(data))
            self.assertEqual(new.read_bytes(), data)
            self.assertFalse(old.exists())
            self.assertEqual(seen[-1], (len(data), len(data)))
            if sys.platform != "win32":
                self.assertTrue(new.stat().st_mode & 0o111)

    def test_a_bad_download_leaves_the_old_one(self):
        with tempfile.TemporaryDirectory() as d:
            old = Path(d) / "SacredFire-v0.5.4-x86_64.AppImage"
            old.write_bytes(b"old")
            release = au.parse_release(release_json(data=b"new appimage"))
            with self.assertRaises(OSError):
                au.install(release, old, opener=lambda req, timeout: _Reply(b"new appimagX"))
            self.assertEqual(old.read_bytes(), b"old")
            self.assertEqual(sorted(p.name for p in Path(d).iterdir()), [old.name])


if __name__ == "__main__":
    unittest.main()
