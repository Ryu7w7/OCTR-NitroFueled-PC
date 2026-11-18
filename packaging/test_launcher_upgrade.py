#!/usr/bin/env python3
"""Exercise the old updater against the built release, without user files or network access."""
import ast
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
        config = home / "config/sacredfire-launcher"
        data.mkdir(parents=True)
        config.mkdir(parents=True)
        # Use every preference from the GTK launcher, including defaults not explicitly changed.
        tree = ast.parse((ROOT / "launcher/sacredfire_launcher.py").read_text())
        defaults = next(node for node in tree.body if isinstance(node, ast.Assign)
                        and any(isinstance(t, ast.Name) and t.id == "DEFAULTS" for t in node.targets))
        scope = {"PACKAGED": True, "PORT_DIR": ROOT}
        exec(compile(ast.Module(body=[defaults], type_ignores=[]), "legacy defaults", "exec"), scope)
        settings = scope["DEFAULTS"] | {"game_dir": str(home / "missing game 日本語"), "user_dir": str(home / "my saves"),
            "ui_language": "id", "gpu": "Test GPU", "gpu_nth": 1, "neo": True, "fullscreen": True,
            "extra_env": "BB_TRACE=1", "discord_rpc": False, "mangohud": True, "dlc_disabled": "test-pack"}
        original = json.dumps(settings, ensure_ascii=False).encode()
        (config / "settings.json").write_bytes(original)
        game = data / "bbport.ini"
        before = "# Player settings\nonline=0\nperf_reports=0\nvolume=42\nkey.cross=Q\npad.cross=b\npatch_60fps=1\n"
        game.write_text(before)
        untouched = {"patches.json": b'{"enabled":["test"]}', "mods.json": b'{"enabled":["test-mod"]}',
                     "ryunet_online.key": b"1" * 64, "user/trophies/test.dat": b"trophies",
                     "backups/save.zip": b"backup", "dlc/test/content": b"dlc"}
        for name, content in untouched.items():
            path = data / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        saves = Path(settings["user_dir"])
        saves.mkdir()
        (saves / "save.dat").write_bytes(b"player save")
        old = home / "SacredFire-v0.8.8-x86_64.AppImage"
        old.write_bytes(b"old launcher")
        installed = appimage_update.install(release, old, opener=lambda *_args, **_kwargs: artifact.open("rb"))
        assert installed == old and not old.with_name(old.name + ".part").exists()
        env = dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(home / "config"), TMPDIR=str(home))
        for name in ("APPIMAGE", "XDG_DATA_HOME", "BB_DATA_DIR", "BB_CONFIG", "BB_USER_DIR"):
            env.pop(name, None)
        def launch(*args):
            return subprocess.run([str(installed), "--appimage-extract-and-run", *args], env=env,
                                  capture_output=True, text=True, timeout=180)
        help_result = launch("--help")
        assert help_result.returncode == 0 and "SacredFire launcher:" in help_result.stdout, help_result.stderr
        first = launch("--updated", "--play")
        assert first.returncode == 1 and "Choose and verify" in first.stderr, first.stderr
        native = dict(line.split("=", 1) for line in (data / "launcher.ini").read_text().splitlines() if "=" in line)
        for key, value in settings.items():
            expected = str(int(value)) if isinstance(value, bool) else str(value)
            assert native[key] == expected, key
        assert (config / "settings.json").read_bytes() == original
        assert game.read_text().startswith(before)
        assert "fullscreen=1" in game.read_text() and "render_res=1440" in game.read_text()
        assert (data / "bbport.ini.pre-native.bak").read_text() == before
        for name, content in untouched.items():
            assert (data / name).read_bytes() == content, name
        assert (saves / "save.dat").read_bytes() == b"player save"
        env.update(BB_DATA_DIR=str(data), BB_CONFIG=str(game), BB_USER_DIR=str(saves))
        (data / "launcher.ini").write_text((data / "launcher.ini").read_text().replace("ui_language=id", "ui_language=fr"))
        updated_settings = (data / "launcher.ini").read_bytes()
        updated_game = game.read_bytes()
        next_release = release | {"tag": "v999.0.1", "name": "SacredFire-v999.0.1-x86_64.AppImage"}
        assert appimage_update.is_newer(appimage_update.parse_version(release["tag"]),
                                       appimage_update.parse_version(next_release["tag"]))
        installed = appimage_update.install(next_release, installed,
                                            opener=lambda *_args, **_kwargs: artifact.open("rb"))
        assert installed == old and not old.with_name(old.name + ".part").exists()
        help_result = launch("--help")
        assert help_result.returncode == 0 and "SacredFire launcher:" in help_result.stdout, help_result.stderr
        assert launch("--updated", "--play").returncode == 1
        assert (data / "launcher.ini").read_bytes() == updated_settings
        assert game.read_bytes() == updated_game
        assert (config / "settings.json").read_bytes() == original
        assert (data / "bbport.ini.pre-native.bak").read_text() == before
        for name, content in untouched.items():
            assert (data / name).read_bytes() == content, name
        assert (saves / "save.dat").read_bytes() == b"player save"
        (data / "launcher.ini").unlink()
        (config / "settings.json").write_text("{")
        broken = launch("--play")
        assert broken.returncode == 1 and "Settings migration failed" in broken.stderr, broken.stderr
        assert not (data / "launcher.ini").exists()
        assert (config / "settings.json").read_text() == "{"
    print("PASS: GTK-to-native and native-to-native updates retain the native launcher, preferences and user files")


if __name__ == "__main__":
    check_upgrade(sys.argv[1], native="--native" in sys.argv[2:])
