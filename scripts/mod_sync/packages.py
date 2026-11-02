"""Validate with OCTR's existing importer, in staging, never in the running game.

SPDX-License-Identifier: GPL-2.0-or-later
"""
from dataclasses import dataclass
import json
from pathlib import Path, PurePosixPath
import stat
import tempfile
import zipfile

import mod_format
import mod_library
from mod_components import safe_path
from .models import MAX_PACKAGE_BYTES, PAGE_FILE, page_id

MAX_EXPANDED_BYTES = 2 * 1024**3


@dataclass(frozen=True)
class Package:
    page_url: str
    manifest: dict | None


def inspect_package(path: Path) -> Package:
    if not 0 < path.stat().st_size <= MAX_PACKAGE_BYTES:
        raise ValueError('Sync ZIP exceeds download limit')
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if not entries or len(entries) > 20000:
            raise ValueError('Invalid sync ZIP member count')
        seen = set()
        total = 0
        page = None
        for entry in entries:
            safe_path(entry.orig_filename.rstrip('/'))
            name = entry.filename.rstrip('/')
            safe_path(name)
            if any(ord(c) < 32 for c in name) or name.casefold() in seen:
                raise ValueError('Duplicate or invalid ZIP member')
            seen.add(name.casefold())
            total += entry.file_size
            if total > MAX_EXPANDED_BYTES:
                raise ValueError('Sync ZIP exceeds expanded size limit')
            if (entry.flag_bits & 1 or stat.S_IFMT(entry.external_attr >> 16) not in
                    (0, stat.S_IFREG, stat.S_IFDIR)):
                raise ValueError('Encrypted/linked/special ZIP member')
            if PurePosixPath(name).name.casefold() == PAGE_FILE:
                if page is not None or entry.is_dir() or entry.file_size > 512:
                    raise ValueError('ZIP needs one small gamebananapageurl.txt')
                text = archive.read(entry).decode('utf-8-sig').strip()
                page_id(text)
                page = (name, f'https://gamebanana.com/mods/{page_id(text)}')
        if page is None:
            raise ValueError('ZIP has no gamebananapageurl.txt')
        inspected = mod_format.inspect_zip(path)
        if inspected:
            manifest, location = inspected
            if PurePosixPath(page[0]).parent != PurePosixPath(location).parent:
                raise ValueError('Put gamebananapageurl.txt beside mod.json')
            if manifest['compatibility']['online'] != 'allowed':
                raise ValueError('Offline-only packages cannot be synced into an online lobby')
        else:
            manifest = None
            # A legacy package has at most one ordinary wrapper, not arbitrary nested tools.
            top = PurePosixPath(page[0]).parent
            if len(top.parts) > 1:
                raise ValueError('Put gamebananapageurl.txt at the package root')
            for entry in entries:
                if entry.is_dir():
                    continue
                if top != PurePosixPath('.') and top not in PurePosixPath(entry.filename).parents:
                    raise ValueError('Files outside the package wrapper')
                if PurePosixPath(entry.filename).suffix.lower() not in ('.pak', '.txt', '.md', '.png', '.jpg', '.jpeg'):
                    raise ValueError('Legacy sync ZIP must contain assets, not executable tools')
        return Package(page[1], manifest)


def validate_package(path: Path, staging_parent: Path) -> Package:
    result = inspect_package(path)
    # Reuse upstream checks for PAK tables, declared file checksums, XML schema and paths.
    with tempfile.TemporaryDirectory(prefix='validate-', dir=staging_parent) as temp:
        dest = Path(temp)
        if result.manifest:
            _, member = mod_format.inspect_zip(path)
            mod_format.unpack_v2(path, dest, member)
        else:
            mod_library.unpack_legacy(path, dest)
    return result


def check_target(path: Path, package: Package, title_id: str, app_version: str):
    """Check every executable-patch alternative before accepting a staged ZIP.

    Assets without executable patches retain upstream's ordinary asset validation.
    Mixed-edition executable alternatives require an explicit future selection
    contract; do not silently select one using filenames or launcher settings.
    """
    if not package.manifest:
        return
    import xml.etree.ElementTree as ET
    with zipfile.ZipFile(path) as archive:
        _, location = mod_format.inspect_zip(path)
        prefix = PurePosixPath(location).parent
        patches = {name for _, manifest in mod_format.expanded_variants(package.manifest)
                   for name in manifest.get('patches', [])}
        for name in patches:
            member = archive.getinfo(str(prefix / name))
            if member.file_size > 8 * 1024**2:
                raise ValueError('Executable patch exceeds sync inspection limit')
            raw = archive.read(member)
            if b'<!DOCTYPE' in raw.upper() or b'<!ENTITY' in raw.upper():
                raise ValueError('XML entities are not supported in synced patches')
            root = ET.fromstring(raw)
            metadata = list(root.iter('Metadata'))
            if (title_id not in ('CUSA13795', 'CUSA14876') or app_version != '01.21' or
                    [node.text for node in root.findall('./TitleID/ID')] != [title_id] or
                    not metadata or any(m.get('AppVer') != app_version for m in metadata)):
                raise ValueError('Executable patch targets a different or unknown game edition/version')
