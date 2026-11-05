#!/usr/bin/env python3
"""One platform-independent release identity embedded in the networking client."""
import hashlib
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def identity(root=ROOT):
    override = os.environ.get('BB_VERSION')
    if override:
        if not re.fullmatch(r'v[0-9]+\.[0-9]+(?:\.[0-9]+)?(?:-[A-Za-z0-9.-]+)?', override) or len(override) > 80:
            raise ValueError('BB_VERSION must be a SacredFire release tag, e.g. v0.8.0')
        return override
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], stderr=subprocess.DEVNULL).decode().strip()
    try:
        tag = git('describe', '--tags', '--exact-match', '--match', 'v*')
        if re.fullmatch(r'v[0-9]+\.[0-9]+(?:\.[0-9]+)?(?:-[A-Za-z0-9.-]+)?', tag) and not git('status', '--porcelain', '--untracked-files=normal', '--ignore-submodules=dirty'):
            return tag
    except subprocess.CalledProcessError:
        pass
    files = sorted(set(root.glob('src/*.[ch]')) | set(root.glob('scripts/**/*.py')) |
                   {root/'build.sh', root/'gpu/CMakeLists.txt', root/'tools/build_version.py'})
    digest = hashlib.sha256()
    for path in files:
        digest.update(path.relative_to(root).as_posix().encode() + b'\0')
        digest.update(path.read_bytes().replace(b'\r\n', b'\n') + b'\0')
    return 'dev-' + digest.hexdigest()[:20]

if __name__ == '__main__':
    print(identity())
