#!/usr/bin/env python3
"""Prepare an existing or downloaded EOS SDK and the maintainer's product configuration."""
import os
from pathlib import Path, PurePosixPath
import shutil
import sys
import tempfile
import urllib.request
import zipfile


def extract_sdk(archive, destination):
    with tempfile.TemporaryDirectory(prefix="eos-sdk-") as temporary:
        with zipfile.ZipFile(archive) as source:
            for entry in source.infolist():
                name = PurePosixPath(entry.filename.replace("\\", "/"))
                if name.is_absolute() or ".." in name.parts or any(":" in part for part in name.parts):
                    raise ValueError("Unsafe SDK archive path")
            source.extractall(temporary)
        headers = list(Path(temporary).glob("**/SDK/Include/eos_sdk.h"))
        if len(headers) != 1:
            raise ValueError("Expected one EOS SDK/Include/eos_sdk.h")
        shutil.copytree(headers[0].parents[1], destination / "SDK", dirs_exist_ok=True)


def prepare_sdk(destination, target):
    config = os.environ.get("EOS_BRIDGE_INI", "")
    contents = config or (target.read_text(encoding="utf-8") if target.is_file() else "")
    settings = {key.strip(): value.strip() for line in contents.splitlines()
                for key, sep, value in [line.partition("=")] if sep}
    missing = [key for key in ("product_id", "sandbox_id", "deployment_id", "client_id", "client_secret")
               if not settings.get(key)]
    if missing:
        raise ValueError("EOS configuration missing: " + ", ".join(missing))
    if not (destination / "SDK/Include/eos_sdk.h").is_file():
        url = os.environ.get("EOS_SDK_URL", "")
        if not url.startswith("https://"):
            raise ValueError("Provide EOS_SDK with SDK/Include/eos_sdk.h or an HTTPS EOS_SDK_URL")
        with tempfile.TemporaryFile() as archive:
            try:
                with urllib.request.urlopen(url, timeout=180) as reply:
                    shutil.copyfileobj(reply, archive)
            except Exception:
                raise SystemExit("EOS SDK download failed; check EOS_SDK_URL") from None
            archive.seek(0)
            extract_sdk(archive, destination)
    if config:
        descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            os.chmod(target, 0o600)
            output.write(config)


def main():
    prepare_sdk(Path(sys.argv[1]), Path(__file__).resolve().parents[1] / "eosbridge/eosbridge.ini")


if __name__ == "__main__":
    main()
