"""launcher/steam_shortcuts.py: the binary VDF round trip, Steam's shortcut app id, adding and
replacing an entry in shortcuts.vdf."""
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

from paths import ROOT

sys.path.insert(0, str(ROOT / "launcher"))
import steam_shortcuts as ss  # noqa: E402


class SteamShortcuts(unittest.TestCase):
    def test_round_trip(self):
        top = {"shortcuts": {"0": ss.make_entry("/games/a.AppImage", "/games", name="A", launch_options="--play")}}
        data = ss.serialize(top)
        self.assertTrue(data.startswith(b"\x00shortcuts\x00\x00" b"0\x00\x02appid\x00"))
        self.assertTrue(data.endswith(b"\x08\x08\x08"))
        self.assertEqual(ss.parse(data), top)
        self.assertEqual(ss.parse(b""), {"shortcuts": {}})

    def test_appid_is_crc_with_top_bit(self):
        exe, name = '"/games/a.AppImage"', "A"
        raw = (zlib.crc32((exe + name).encode()) | 0x80000000) & 0xFFFFFFFF
        appid = ss.shortcut_appid(exe, name)
        self.assertEqual(struct.unpack("<I", struct.pack("<i", appid))[0], raw)
        self.assertEqual(ss.make_entry("/games/a.AppImage", "/games", name="A")["appid"], appid)

    def test_add_then_replace(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "config/shortcuts.vdf"
            other = {"appid": 5, "AppName": "Other", "Exe": '"/x"', "StartDir": '"/"', "tags": {"0": "favorite"}}
            path.parent.mkdir()
            path.write_bytes(ss.serialize({"shortcuts": {"0": other}}))
            entry = ss.make_entry("/games/a.AppImage", "/games", launch_options="--play")
            self.assertTrue(ss.add_shortcut(path, entry))
            top = ss.parse(path.read_bytes())
            self.assertEqual(top["shortcuts"]["0"], other)
            self.assertEqual(top["shortcuts"]["1"], entry)
            self.assertEqual(top["shortcuts"]["1"]["LaunchOptions"], "--play")
            # The same executable and name again: the entry is replaced, not duplicated.
            entry2 = ss.make_entry("/games/a.AppImage", "/games", launch_options="")
            self.assertFalse(ss.add_shortcut(path, entry2))
            top = ss.parse(path.read_bytes())
            self.assertEqual(sorted(top["shortcuts"]), ["0", "1"])
            self.assertEqual(top["shortcuts"]["1"]["LaunchOptions"], "")
            self.assertFalse(path.with_name(path.name + ".tmp").exists())

    def test_older_release_entry_is_updated(self):
        """An entry for an older release's AppImage (another versioned file name) is updated in
        place, keeping its app id (Steam's artwork) and play time; other games are untouched."""
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "config/shortcuts.vdf"
            old = ss.make_entry("/games/SacredFire-v0.5-x86_64.AppImage", "/games", launch_options="--play")
            old["LastPlayTime"] = 1234
            other = ss.make_entry("/games/other.AppImage", "/games", name="Other")
            path.parent.mkdir()
            path.write_bytes(ss.serialize({"shortcuts": {"0": old, "1": other}}))
            new = ss.make_entry("/data/SacredFire.AppImage", "/data", launch_options="--play")
            self.assertFalse(ss.add_shortcut(path, new))
            top = ss.parse(path.read_bytes())
            self.assertEqual(sorted(top["shortcuts"]), ["0", "1"])
            self.assertEqual(top["shortcuts"]["0"]["Exe"], '"/data/SacredFire.AppImage"')
            self.assertEqual(top["shortcuts"]["0"]["appid"], old["appid"])
            self.assertEqual(top["shortcuts"]["0"]["LastPlayTime"], 1234)
            self.assertEqual(top["shortcuts"]["1"], other)

    @unittest.skipUnless(hasattr(__import__("os"), "symlink") and sys.platform != "win32", "symlinks")
    def test_stable_appimage_link(self):
        import os
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as temp:
            data = Path(temp) / "data"
            first, second = Path(temp) / "SacredFire-v0.5.1-x86_64.AppImage", Path(temp) / "SacredFire-v0.5.2-x86_64.AppImage"
            first.write_bytes(b"1")
            second.write_bytes(b"2")
            with patch.dict(os.environ, {"APPIMAGE": str(first), "BB_DATA_DIR": str(data)}):
                link = ss.stable_appimage_link()
                self.assertEqual(Path(link).resolve(), first.resolve())
                self.assertEqual(ss.launcher_command()[0], link)
            with patch.dict(os.environ, {"APPIMAGE": str(second), "BB_DATA_DIR": str(data)}):
                self.assertEqual(Path(ss.stable_appimage_link()).resolve(), second.resolve())
            with patch.dict(os.environ, {}, clear=False):
                os.environ.pop("APPIMAGE", None)
                self.assertIsNone(ss.stable_appimage_link())

    def test_launcher_command(self):
        exe, start_dir, options = ss.launcher_command()
        self.assertEqual(options, "--play")
        self.assertEqual(start_dir, str(Path(exe).parent))
        self.assertTrue(exe.endswith("bb-launcher.sh") or exe.endswith(".AppImage"))


if __name__ == "__main__":
    unittest.main()
