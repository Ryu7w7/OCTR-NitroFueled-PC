"""NVIDIA devices are prepared before the namespace entry point is executed."""
from paths import ROOT
from pathlib import Path
import json
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="appimage bootstrap ")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.proc = self.root / "proc"
        self.dev = self.root / "dev"
        self.proc.mkdir()
        self.dev.mkdir()
        self.helper = self.root / "nvidia-modprobe"
        self.calls = self.root / "calls"
        self.helper.write_text(
            "#!/usr/bin/env python3\nimport pathlib,sys\n"
            f"root=pathlib.Path({str(self.root)!r})\n"
            "with (root/'calls').open('a') as f: f.write(' '.join(sys.argv[1:])+'\\n')\n"
            "name='nvidia-modeset' if sys.argv[1]=='-m' else ('nvidiactl' if sys.argv[2]=='255' else 'nvidia'+sys.argv[2])\n"
            "(root/'dev'/name).symlink_to('/dev/null')\n")
        self.helper.chmod(0o755)
        target = self.root / "AppRun.nix"
        target.write_text(
            "#!/usr/bin/env python3\nimport pathlib,sys,json\n"
            f"root=pathlib.Path({str(self.root)!r})\n"
            "print(json.dumps({'args':sys.argv[1:],'devices':sorted(p.name for p in (root/'dev').iterdir())}))\n")
        target.chmod(0o755)
        self.binary = self.root / "AppRun"
        subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror",
                        f'-DNVIDIA_PROC="{self.proc}"', f'-DNVIDIA_DEV="{self.dev}"',
                        f'-DNVIDIA_HELPER="{self.helper}"',
                        str(ROOT / "packaging/appimage-bootstrap.c"), "-o", str(self.binary)], check=True)

    def driver(self, minors=(0,)):
        (self.proc / "version").touch()
        for minor in minors:
            directory = self.proc / "gpus" / str(minor)
            directory.mkdir(parents=True)
            (directory / "information").write_text(f"Model: NVIDIA\nDevice Minor: {minor}\n")

    def run_bootstrap(self):
        result = subprocess.run([str(self.binary), "--play", "two words"],
                                capture_output=True, text=True, check=True)
        return json.loads(result.stdout)

    def test_cold_start_prepares_all_devices_before_launch(self):
        self.driver((0, 2))
        result = self.run_bootstrap()
        self.assertEqual(result["args"], ["--play", "two words"])
        self.assertEqual(result["devices"], ["nvidia-modeset", "nvidia0", "nvidia2", "nvidiactl"])
        self.assertEqual(set(self.calls.read_text().splitlines()), {"-c 255", "-c 0", "-c 2", "-m"})

    def test_initialized_driver_does_not_run_helper_again(self):
        self.driver()
        for name in ("nvidiactl", "nvidia0", "nvidia-modeset"):
            (self.dev / name).symlink_to('/dev/null')
        self.run_bootstrap()
        self.assertFalse(self.calls.exists())

    def test_non_nvidia_system_starts_without_helper(self):
        self.assertEqual(self.run_bootstrap()["devices"], [])
        self.assertFalse(self.calls.exists())

    def test_helper_failure_does_not_prevent_other_gpus_from_starting(self):
        self.driver()
        self.helper.write_text("#!/bin/sh\nexit 1\n")
        self.assertEqual(self.run_bootstrap()["devices"], [])
