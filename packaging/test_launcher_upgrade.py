#!/usr/bin/env python3
"""Exercise native AppImage updates without network access or user files."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "launcher"))
import appimage_update


def check_upgrade(artifact, native=False):
    artifact = Path(artifact).resolve()
    with artifact.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    release = appimage_update.parse_release(json.dumps({"tag_name": "v999.0.0", "assets": [{
        "name": "SacredFire-v999.0.0-x86_64.AppImage", "browser_download_url": "https://example.invalid/release",
        "size": artifact.stat().st_size, "digest": "sha256:" + digest}]}))
    assert release and appimage_update.is_newer(appimage_update.parse_version("v0.8.8"),
                                                appimage_update.parse_version(release["tag"]))
    with tempfile.TemporaryDirectory(prefix="launcher upgrade ") as tmp:
        home = Path(tmp)
        if native:
            for name in ("run.py", "launcher", "scripts", "patches", "art"):
                (home / name).symlink_to(ROOT / name)
        data = home / ".local/share/sacredfire"
        data.mkdir(parents=True)
        settings = data / "launcher.ini"
        settings.write_text("game_dir=/missing game 日本語\nui_language=id\n")
        game = data / "bbport.ini"
        game.write_text("online=0\nvolume=42\nkey.cross=Q\n")
        untouched = {"patches.json": b'{"enabled":["test"]}', "mods.json": b'{"enabled":["test-mod"]}',
                     "ryunet_online.key": b"1" * 64, "user/trophies/test.dat": b"trophies",
                     "backups/save.zip": b"backup", "dlc/test/content": b"dlc"}
        for name, content in untouched.items():
            path = data / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        old = home / "SacredFire-v0.8.8-x86_64.AppImage"
        old.write_bytes(b"old launcher")
        env = dict(os.environ, HOME=str(home), BB_DATA_DIR=str(data), TMPDIR=str(home))
        for name in ("APPIMAGE", "XDG_DATA_HOME", "BB_CONFIG", "BB_USER_DIR"):
            env.pop(name, None)

        def launch(*args):
            return subprocess.run([str(old), "--appimage-extract-and-run", *args], env=env,
                                  capture_output=True, text=True, timeout=180)

        for version in ("v999.0.0", "v999.0.1"):
            release["tag"] = version
            installed = appimage_update.install(release, old, opener=lambda *_args, **_kwargs: artifact.open("rb"))
            assert installed == old and not old.with_name(old.name + ".part").exists()
            help_result = launch("--help")
            assert help_result.returncode == 0 and "SacredFire launcher:" in help_result.stdout, help_result.stderr
            result = launch("--updated", "--play")
            assert result.returncode == 1 and "Choose and verify" in result.stderr, result.stderr
            assert settings.read_text() == "game_dir=/missing game 日本語\nui_language=id\n"
            assert game.read_text() == "online=0\nvolume=42\nkey.cross=Q\n"
            for name, content in untouched.items():
                assert (data / name).read_bytes() == content, name
    print("PASS: native AppImage updates preserve launcher settings and user files")


if __name__ == "__main__":
    check_upgrade(sys.argv[1], "--native" in sys.argv[2:])
