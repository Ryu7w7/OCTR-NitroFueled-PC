#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The port's optional parts, downloaded the first time they are needed instead of shipped in every package
(27/11: the package went from 145 MB to about 50 MB):

  dlss  bin/nvngx_dlss.dll (NVIDIA DLSS, 59 MB): when DLSS is chosen (bbport.ini temporal=dlss); only RTX cards use it
  dxc   bin/dxc/dxc.exe + dxcompiler.dll (the DirectX shader compiler, 24 MB): when the native renderer compiles
        its shaders the first time (scripts/native_shaders.py)

Each file is pinned here by size and SHA-256 and taken from the port's mirror (the Cloudflare R2 bucket that holds
the tracks: <mirror>components/<sha256>/<name>); a file that does not match is never used. A package that still
has a file (an older one, or a build with --bundle-components) downloads nothing.

    components.py ensure dlss|dxc [--root PORT]      what the game does; exit 0 when the part is there
"""
import argparse
import hashlib
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MIRROR = 'https://dlctr.ryu7w7.xyz/components/'
USER_AGENT = 'SacredFire-content/1'   # Cloudflare refuses Python's own (scripts/track_content.py)

# name: [(path in the package, size, sha256)]
COMPONENTS = {
    'dlss': [('bin/nvngx_dlss.dll', 58956912, 'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e')],
    'dxc': [('bin/dxc/dxc.exe', 1359640, 'aa7965b3763d9c710baaa5a28cedcc633449962a75d1391d161a85304a41b3e2'),
            ('bin/dxc/dxcompiler.dll', 22274872, '0689e2fee0a1c9df8662a59680469f24e2a3ad7ce6bbbcc9284f7a83172bfc59')],
}
TITLES = {'dlss': 'NVIDIA DLSS', 'dxc': 'the shader compiler (DXC)'}


def mirror():
    url = os.environ.get('BB_COMPONENTS_URL') or MIRROR
    return url if url.endswith('/') else url + '/'


def file_sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def present(name, root=ROOT):
    """Whether every file of a part is there, at its size (a size that differs: not the pinned file)."""
    return all((Path(root) / path).is_file() and (Path(root) / path).stat().st_size == size
               for path, size, _sha in COMPONENTS[name])


def download(url, target, size, sha, log, title):
    import urllib.request
    part = target.with_name(target.name + '.part')
    target.parent.mkdir(parents=True, exist_ok=True)
    h, done, shown = hashlib.sha256(), 0, -1
    request = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
    with urllib.request.urlopen(request, timeout=30) as reply, open(part, 'wb') as out:
        while True:
            block = reply.read(1 << 20)
            if not block:
                break
            out.write(block)
            h.update(block)
            done += len(block)
            percent = done * 100 // max(size, 1)
            if percent // 10 != shown // 10:
                shown = percent
                log(f'@progress components {min(percent, 100)} {title}')
            if done > size:
                break
    if done != size or h.hexdigest() != sha:
        part.unlink(missing_ok=True)
        raise ValueError(f'{target.name}: the download is not the expected file ({done} bytes)')
    os.replace(part, target)


def ensure(name, root=ROOT, log=print, tries=2):
    """True when the part is there (downloaded now if it was not). Never raises: a part that cannot be had
    leaves the game without it (DLSS off, the native renderer not prepared) and says why."""
    if name not in COMPONENTS:
        return False
    if present(name, root):
        return True
    # Windows files: the Linux package (the AppImage) has its own DXC (BB_DXC) and no DLSS DLL to fetch.
    if os.name != 'nt':
        return False
    if present(name, root):
        return True
    title = TITLES.get(name, name)
    total = sum(size for _p, size, _s in COMPONENTS[name])
    log(f'Components: downloading {title} ({total / 1e6:.0f} MB, once)')
    for path, size, sha in COMPONENTS[name]:
        target = Path(root) / path
        if target.is_file() and target.stat().st_size == size and file_sha(target) == sha:
            continue
        error = None
        for _ in range(tries):
            try:
                download(f'{mirror()}{sha}/{Path(path).name}', target, size, sha, log, title)
                error = None
                break
            except (OSError, ValueError) as e:
                error = e
        if error:
            log(f'Components: {title} could not be downloaded ({error}); the game goes on without it')
            return False
    log(f'Components: {title} ready')
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('action', choices=('ensure',))
    p.add_argument('name', choices=sorted(COMPONENTS))
    p.add_argument('--root', type=Path, default=ROOT)
    a = p.parse_args()
    sys.exit(0 if ensure(a.name, a.root, log=lambda s: print(s, flush=True)) else 1)


if __name__ == '__main__':
    main()
