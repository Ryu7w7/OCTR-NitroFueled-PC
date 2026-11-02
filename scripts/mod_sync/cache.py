"""Quarantined download cache and explicit, idle-only import into OCTR.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import hashlib
from pathlib import Path
import os
import shutil
import threading
import uuid

import mod_library
from mod_components import save_json, sha
from mod_session import launch_lock
from .gamebanana import GameBanana
from .models import MAX_CACHE_BYTES, Offer, page_id
from .packages import MAX_EXPANDED_BYTES, validate_package, check_target
from .transport import Cancelled


class Cache:
    def __init__(self, root: Path, api=None, progress=None, *, title_id="", app_version=""):
        self.root = Path(root)
        self.api = api or GameBanana()
        self.progress = progress
        self.title_id = title_id
        self.app_version = app_version

    def stage(self, offer: Offer, cancelled: threading.Event):
        self.root.mkdir(parents=True, exist_ok=True)
        # Serialize quota checks and validation across runners sharing this data directory.
        with launch_lock(self.root / 'transfer'):
            return self._stage(offer, cancelled)

    def _stage(self, offer: Offer, cancelled: threading.Event):
        if cancelled.is_set():
            raise Cancelled()
        upload = self.api.for_title(offer, self.title_id)
        original = upload.file_id == offer.file_id
        target = self.root / offer.cache_name
        # Re-resolve even on cache hits; removed/quarantined uploads are rejected.
        # Regional alternatives receive their OWN hash receipt after download.
        # Never report the source hash as loaded for an alternative.
        if original and target.exists() and not target.is_symlink() and target.stat().st_size == offer.bytes and sha(target) == offer.sha256:
            with target.open('rb') as stream:
                if hashlib.file_digest(stream, 'md5').hexdigest() == upload.md5:
                    package = validate_package(target, self.root)
                    check_target(target, package, self.title_id, self.app_version)
                    if page_id(package.page_url) != page_id(offer.page_url):
                        raise ValueError('Cached package page mismatch')
                    save_json(self.root / (offer.sha256 + '.json'), offer.to_dict())
                    return target
        used = sum(p.stat().st_size for p in self.root.iterdir() if p.is_file())
        if used + upload.size > MAX_CACHE_BYTES:
            raise ValueError('Sync cache is full; review/remove cached ZIPs before downloading more')
        if shutil.disk_usage(self.root).free < upload.size + MAX_EXPANDED_BYTES + 128 * 1024**2:
            raise ValueError('Not enough space for download and validation')
        partial = self.root / (uuid.uuid4().hex + '.part')
        try:
            digest = self.api.transport.download(upload.url, partial, upload.size, cancelled, self.progress)
            if original and digest != offer.sha256:
                raise ValueError('Downloaded ZIP does not match the advertised SHA-256')
            with partial.open('rb') as stream:
                if hashlib.file_digest(stream, 'md5').hexdigest() != upload.md5:
                    raise ValueError('Downloaded ZIP does not match GameBanana metadata')
            package = validate_package(partial, self.root)
            check_target(partial, package, self.title_id, self.app_version)
            if package.page_url != f'https://gamebanana.com/mods/{page_id(offer.page_url)}':
                raise ValueError('ZIP names a different GameBanana page')
            if cancelled.is_set():
                raise Cancelled()
            offer = Offer(offer.page_url, upload.file_id, upload.size, digest)
            target = self.root / offer.cache_name
            os.replace(partial, target)
            save_json(self.root / (offer.sha256 + '.json'), offer.to_dict())
            return target
        finally:
            partial.unlink(missing_ok=True)

    def import_for_review(self, offer: Offer, mods: Path, patchdir: Path):
        """No auto-enable, active session writes, or silent upgrades of an installed ID."""
        mods = Path(mods)
        with launch_lock(mods.parent / 'mod-state/sync-runtime'), launch_lock(mods.parent / 'mod-state'):
            source = self.root / offer.cache_name
            if source.is_symlink() or source.stat().st_size != offer.bytes or sha(source) != offer.sha256:
                raise ValueError('Cached ZIP changed; download it again')
            package = validate_package(source, self.root)
            check_target(source, package, self.title_id, self.app_version)
            if page_id(package.page_url) != page_id(offer.page_url):
                raise ValueError('Cached package page mismatch')
            catalog = mod_library.scan(mods, patchdir)
            for existing in catalog['entries'].values():
                if existing['sha256'] == offer.sha256:
                    return mods / existing['source']
            if package.manifest and package.manifest['id'] in catalog['entries']:
                raise ValueError('This package ID is already installed; review the update manually')
            name = f'gb-{page_id(offer.page_url)}-{offer.file_id}-{offer.sha256[:12]}.zip'
            dest = mods / name
            # Exclusive create: never replace another user's mod or follow an existing link.
            with dest.open('xb') as output, source.open('rb') as input_stream:
                shutil.copyfileobj(input_stream, output)
            try:
                scanned = mod_library.scan(mods, patchdir)
                if not any(i['source'] == name for i in scanned['entries'].values()):
                    raise ValueError('OCTR importer rejected the package')
                # An old remembered enabled ID must not silently activate a newly imported ZIP.
                selection = mods.parent / 'patches.json'
                import json
                settings = json.loads(selection.read_text()) if selection.exists() else {}
                ids = [ident for ident, item in scanned['entries'].items() if item['source'] == name]
                enabled = settings.get('enabled', [])
                disabled = settings.get('disabled', [])
                if not isinstance(enabled, list) or not isinstance(disabled, list):
                    raise ValueError('Invalid patch selection')
                keys = [mod_library.PREFIX + ident for ident in ids]
                settings['enabled'] = [key for key in enabled if key not in keys]
                settings['disabled'] = list(dict.fromkeys(disabled + keys))
                save_json(selection, settings)
                return dest
            except Exception:
                dest.unlink(missing_ok=True)
                mod_library.scan(mods, patchdir)
                raise
