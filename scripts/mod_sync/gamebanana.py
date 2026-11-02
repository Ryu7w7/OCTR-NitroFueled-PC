"""Resolve exact official API uploads; never scrape HTML or guess newest versions.

SPDX-License-Identifier: GPL-2.0-or-later
"""
from dataclasses import dataclass
import hashlib
from pathlib import Path
import re

from .models import MAX_PACKAGE_BYTES, Offer, page_id, positive_id
from .packages import inspect_package
from .transport import Transport


@dataclass(frozen=True)
class Upload:
    file_id: int
    size: int
    md5: str
    url: str
    title_ids: tuple[str, ...] = ()
    version: str = ""


class GameBanana:
    def __init__(self, transport=None):
        self.transport = transport or Transport()

    def uploads(self, page_url):
        ident = page_id(page_url)
        data = self.transport.json(f'https://gamebanana.com/apiv11/Mod/{ident}/ProfilePage')
        if (not isinstance(data, dict) or data.get('_idRow') != ident or
                data.get('_bIsTrashed') or data.get('_bIsWithheld') or data.get('_sErrorCode')):
            raise ValueError('GameBanana mod is unavailable')
        # This integration is for the PC game; do not sync unrelated games' packages.
        if data.get('_aGame', {}).get('_idRow') != 26138:
            raise ValueError('GameBanana page is not in the OCTR PC game category')
        files = data.get('_aFiles')
        if not isinstance(files, list) or len(files) > 256:
            raise ValueError('Unexpected GameBanana file list')
        result = []
        seen = set()
        for entry in files:
            if not isinstance(entry, dict):
                raise ValueError('Invalid GameBanana upload')
            ident, size = entry.get('_idRow'), entry.get('_nFilesize')
            if not positive_id(ident) or ident in seen:
                raise ValueError('Invalid/duplicate GameBanana upload ID')
            seen.add(ident)
            if entry.get('_bIsArchived'):
                continue
            if type(size) is not int or not 0 < size <= MAX_PACKAGE_BYTES:
                continue
            if not str(entry.get('_sFile', '')).lower().endswith('.zip'):
                continue
            md5 = entry.get('_sMd5Checksum', '')
            if not isinstance(md5, str) or not re.fullmatch('[0-9a-fA-F]{32}', md5):
                continue
            # Explicit failed/pending checks are not treated as a clean upload.
            if entry.get('_sAvResult') != 'clean' or entry.get('_sAnalysisResult') != 'ok':
                continue
            url = f'https://gamebanana.com/dl/{ident}'
            if entry.get('_sDownloadUrl') != url:
                raise ValueError('Unexpected GameBanana download URL')
            description = entry.get('_sDescription', '')
            version = entry.get('_sVersion', '')
            if not isinstance(description, str) or not isinstance(version, str):
                raise ValueError('Invalid upload compatibility metadata')
            titles = tuple(sorted(set(re.findall(r'\bCUSA[0-9]{5}\b', description))))
            result.append(Upload(ident, size, md5.lower(), url, titles, version.strip()))
        return result

    def resolve(self, offer: Offer):
        matches = [f for f in self.uploads(offer.page_url) if f.file_id == offer.file_id]
        if len(matches) != 1 or matches[0].size != offer.bytes:
            raise ValueError('The exact advertised upload is no longer available')
        return matches[0]

    def for_title(self, offer: Offer, title_id: str):
        """Select only an explicit, unique same-release edition on the SAME page.

        Descriptions must contain title IDs; filenames and USA/EU nicknames are
        never evidence. This chooses a staging candidate, not runtime equivalence.
        The downloaded XML must independently match the actual game edition.
        """
        files = self.uploads(offer.page_url)
        sources = [f for f in files if f.file_id == offer.file_id and f.size == offer.bytes]
        if len(sources) != 1:
            raise ValueError('Advertised upload is unavailable')
        source = sources[0]
        if source.title_ids and title_id not in ('CUSA13795', 'CUSA14876'):
            raise ValueError('Cannot resolve a regional mod without a supported game edition')
        if not source.title_ids or title_id in source.title_ids:
            return source
        if not source.version:
            raise ValueError('Regional upload needs an explicit release version')
        matches = [f for f in files if f.title_ids == (title_id,) and f.version == source.version]
        if len(matches) != 1:
            raise ValueError('No unique same-release upload for this game edition')
        return matches[0]

    def pin(self, package: Path):
        """Owner advertises only an installed ZIP identical to a public upload.

        The API's MD5 identifies an upload; the receiver still verifies SHA-256.
        Never embed the outer ZIP's digest inside itself (circular dependency).
        """
        inspected = inspect_package(package)
        md5 = hashlib.md5(usedforsecurity=False)
        sha = hashlib.sha256()
        with package.open('rb') as stream:
            for chunk in iter(lambda: stream.read(128 * 1024), b''):
                sha.update(chunk)
                md5.update(chunk)
        size = package.stat().st_size
        matches = [f for f in self.uploads(inspected.page_url) if f.size == size and f.md5 == md5.hexdigest()]
        if len(matches) != 1:
            raise ValueError('Local ZIP must match exactly one public GameBanana upload; do not repack it')
        return Offer(inspected.page_url, matches[0].file_id, size, sha.hexdigest())
