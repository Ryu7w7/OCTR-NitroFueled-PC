#!/usr/bin/env python3
"""Export a committed source overlay and patch against the pinned OCTR baseline.

SPDX-License-Identifier: GPL-2.0-or-later
No binaries, game files, local caches, credentials or user configuration are copied.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
BASE = '3885d1aea24d19ed9b31f96fba57a5cce92183cd'


def git(*args, cwd=ROOT):
    return subprocess.check_output(['git', *args], cwd=cwd)


def build(output):
    if git('status', '--porcelain').strip():
        raise ValueError('Commit the reviewed source before packaging')
    head = git('rev-parse', 'HEAD').decode().strip()
    files = git('diff', '--name-only', '--diff-filter=ACM', BASE, head).decode().splitlines()
    patch = git('diff', '--binary', '--full-index', BASE, head)
    # Verify against a fresh exported upstream tree, not this already-modified checkout.
    checks = ROOT / 'out/mod-sync-tests'
    checks.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='patch-check-', dir=checks) as temporary:
        folder = Path(temporary)
        archive = git('archive', '--format=zip', BASE)
        with zipfile.ZipFile(io.BytesIO(archive)) as source:
            for info in source.infolist():
                parts = PurePosixPath(info.filename)
                if parts.is_absolute() or '..' in parts.parts:
                    raise ValueError('Unexpected upstream archive path')
            source.extractall(folder)
        patch_file = folder / 'handoff.patch'
        patch_file.write_bytes(patch)
        git('init', '-q', cwd=folder)
        git('apply', '--check', '--whitespace=error-all', str(patch_file), cwd=folder)
        git('apply', str(patch_file), cwd=folder)
        for name in files:
            if (folder / name).read_bytes() != git('show', f'{head}:{name}'):
                raise ValueError('Applied patch differs from source: ' + name)

    readme = f'''OCTR GAMEBANANA MOD SYNC — FORK SOURCE HANDOFF
Base: Ryu7w7/OCTR-NitroFueled-PC v0.7.5
Base commit: {BASE}
Source commit: {head}

This is an integration candidate for developers, NOT a player mod ZIP.
Do not add this ZIP to the launcher's Mods page or overwrite a playable installation.

OPTION A — RECOMMENDED: APPLY THE PATCH TO YOUR FORK
1. Clone your fork of https://github.com/Ryu7w7/OCTR-NitroFueled-PC
2. Check out a new branch based on v0.7.5.
3. In that checkout run:
   git apply --check "PATH/TO/OCTR-GameBanana-ModSync.patch"
   git apply "PATH/TO/OCTR-GameBanana-ModSync.patch"
4. Review the diff, commit it to your fork, and follow docs/mod-sync-integration.txt.
The patch was applied and byte-verified against a clean export of the exact base.
If your fork has advanced, use --check and resolve differences; do not force overwrite.

OPTION B — COPY THE SOURCE OVERLAY
Copy the CONTENTS of source-overlay into the ROOT of your fork's checkout.
Its scripts/, launcher/, gpu/, tests/, docs/ and integration/ paths match the repo.
Use a clean branch: copying files may replace your fork's own changes.

WHAT STILL NEEDS CONNECTING
The new Go component under integration/ryunet-modsync belongs in the online SERVER.
Ryu must connect current authentication, membership and the actual countdown gate.
Newly downloaded mods remain staged; runtime loaded-proof/hot-load or a deliberate
restart/rejoin flow must be connected before claiming seamless same-lobby syncing.
The original sync bar design is retained. Focus-independent drawing passes a
headless ImGui test; actual Alt+Tab/fullscreen behaviour still needs an in-game test.
Windows launcher/run.py are wired; Linux GTK/run.sh integration remains outstanding.

Read these files first:
  source-overlay/docs/mod-sync-integration.txt
  source-overlay/docs/mod-sync-quickstart-en.txt
  source-overlay/docs/mod-sync-quickstart-es.txt
  source-overlay/docs/mod-sync-validation.txt

All changes are source/tests/docs. No game, engine-rewrite work, saves, keys or
installed mods are bundled. Keep LICENSE and upstream notices. No GitHub publishing
or server deployment has been performed automatically.
'''.encode('utf-8')
    payloads = {'README-FIRST.txt': readme, 'OCTR-GameBanana-ModSync.patch': patch,
                'LICENSE': git('show', f'{head}:LICENSE')}
    for name in files:
        payloads['source-overlay/' + name] = git('show', f'{head}:{name}')
    manifest = {'base': BASE, 'source': head, 'patch_applied_and_verified': True,
                'files': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, data in sorted(payloads.items())}}
    payloads['SOURCE-MANIFEST.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'x', zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(payloads.items()):
            archive.writestr(name, data)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:
            raise ValueError('ZIP verification failed')
        for name, data in payloads.items():
            if archive.read(name) != data:
                raise ValueError('ZIP content verification failed')
    print(json.dumps({'zip': str(output.resolve()), 'files': len(payloads),
        'bytes': output.stat().st_size, 'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
        'source': head, 'patch_applied_and_verified': True}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    build(parser.parse_args().output)
