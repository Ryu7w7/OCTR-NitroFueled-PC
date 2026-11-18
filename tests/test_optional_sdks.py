"""Optional SDK downloads keep platforms separate and reject unsafe private archives."""
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

from paths import ROOT
import fetch_dlss_sdk
from fetch_eos_sdk import extract_sdk, prepare_sdk

EOS_CONFIG = "".join(f"{key}=test-{key}\n" for key in
                     ("product_id", "sandbox_id", "deployment_id", "client_id", "client_secret"))


class OptionalSdkTests(unittest.TestCase):
    def test_dlss_manifest_is_pinned_and_platform_specific(self):
        self.assertEqual(len(fetch_dlss_sdk.REVISION), 40)
        linux, windows = (fetch_dlss_sdk.files(p) for p in ("linux", "windows"))
        self.assertIn("LICENSE.txt", linux)
        self.assertTrue(any(name.endswith(".dll") for name in windows))
        self.assertFalse(any("Windows" in name for name in linux))
        self.assertFalse(any("Linux" in name for name in windows))
        with self.assertRaises(ValueError):
            fetch_dlss_sdk.files("unsupported")

    def test_eos_archive_root_and_path_validation(self):
        with tempfile.TemporaryDirectory() as folder:
            archive = io.BytesIO()
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("EOS-SDK/SDK/Include/eos_sdk.h", "test SDK")
            archive.seek(0)
            extract_sdk(archive, Path(folder))
            self.assertEqual((Path(folder) / "SDK/Include/eos_sdk.h").read_text(), "test SDK")
            for name in ("../escape", "/absolute", "C:/escape", "folder\\..\\escape"):
                archive = io.BytesIO()
                with zipfile.ZipFile(archive, "w") as output:
                    output.writestr(name, "invalid")
                archive.seek(0)
                with self.assertRaises(ValueError):
                    extract_sdk(archive, Path(folder))

    def test_eos_prepare_download_cache_and_configuration(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            archive = io.BytesIO()
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("EOS-SDK/SDK/Include/eos_sdk.h", "test SDK")
            archive.seek(0)
            target = root / "eosbridge.ini"
            with mock.patch.dict(os.environ, {"EOS_SDK_URL": "https://example.invalid/sdk.zip",
                                              "EOS_BRIDGE_INI": EOS_CONFIG}, clear=True), \
                 mock.patch("fetch_eos_sdk.urllib.request.urlopen", return_value=archive) as download:
                prepare_sdk(root, target)
                prepare_sdk(root, target)
                download.assert_called_once()
            self.assertEqual(target.read_text(), EOS_CONFIG)
            if os.name != "nt":
                self.assertEqual(target.stat().st_mode & 0o777, 0o600)
            with mock.patch.dict(os.environ, {}, clear=True), \
                 mock.patch("fetch_eos_sdk.urllib.request.urlopen") as download:
                prepare_sdk(root, target)
                download.assert_not_called()
            with mock.patch.dict(os.environ, {"EOS_BRIDGE_INI": "product_id=only-one-key"}, clear=True):
                with self.assertRaisesRegex(ValueError, "configuration missing"):
                    prepare_sdk(root, target)
            self.assertEqual(target.read_text(), EOS_CONFIG)

    @unittest.skipUnless(os.name == "posix" and shutil.which("cc") and shutil.which("bash"), "needs Linux build tools")
    def test_eos_auto_build_skips_unconfigured_and_builds_configured_sdk(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in ("eosbridge/build.sh", "eosbridge/embed_settings.py", "scripts/fetch_eos_sdk.py"):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / name, target)
            env = {key: value for key, value in os.environ.items() if not key.startswith("EOS_") and key != "CC"}
            env["HOME"] = str(root)
            command = ["bash", str(root / "eosbridge/build.sh"), "--auto"]
            skipped = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertEqual(skipped.returncode, 0, skipped.stderr)
            self.assertIn("EOS is not configured", skipped.stdout)
            self.assertFalse((root / "out/eosbridge").exists())
            sdk = root / "sdk"
            (sdk / "SDK/Include").mkdir(parents=True)
            (sdk / "SDK/Bin").mkdir()
            (sdk / "SDK/Include/eos_sdk.h").write_text("/* build fixture */\n")
            runtime = "libEOSSDK-LinuxArm64-Shipping.so" if os.uname().machine == "aarch64" else "libEOSSDK-Linux-Shipping.so"
            (sdk / "SDK/Bin" / runtime).write_bytes(b"runtime fixture")
            (root / "eosbridge/eosbridge.c").write_text(
                '#include "eos_sdk.h"\n#include EOS_EMBEDDED\n'
                'int main(void) { return sizeof(eos_embedded) > 1 ? 0 : 1; }\n')
            env.update(EOS_SDK=str(sdk), EOS_BRIDGE_INI=EOS_CONFIG)
            built = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            self.assertEqual(subprocess.run([str(root / "out/eosbridge/ryunet-bridge")]).returncode, 0)
            self.assertEqual((root / "out/eosbridge" / runtime).read_bytes(), b"runtime fixture")
            env["EOS_BRIDGE_INI"] = "product_id=incomplete"
            failed = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("configuration missing", failed.stderr)
