"""Small, versioned wire objects; remote strings never become filesystem paths.

SPDX-License-Identifier: GPL-2.0-or-later
"""
from dataclasses import asdict, dataclass
import re

MAX_PACKAGE_BYTES = 512 * 1024**2
MAX_CACHE_BYTES = 2 * 1024**3
MAX_OFFERS = 32
PAGE_FILE = 'gamebananapageurl.txt'


def page_id(url: str) -> int:
    if not isinstance(url, str):
        raise ValueError('Expected a GameBanana mod page URL')
    match = re.fullmatch(r'https://(?:www\.)?gamebanana\.com/mods/([1-9][0-9]{0,9})/?', url)
    if not match:
        raise ValueError('Use https://gamebanana.com/mods/NUMBER without a query or fragment')
    return int(match[1])


def positive_id(value: int) -> bool:
    return type(value) is int and 0 < value < 10**10


@dataclass(frozen=True)
class Offer:
    """Pins one upload, not the mutable 'latest file' on a page.

    SHA-256 is an integrity identity, not a publisher signature or a safety verdict.
    The server must authenticate the advertiser and scope offers to their lobby.
    """
    page_url: str
    file_id: int
    bytes: int
    sha256: str

    def __post_init__(self):
        page_id(self.page_url)
        if not positive_id(self.file_id):
            raise ValueError('Invalid GameBanana file ID')
        if type(self.bytes) is not int or not 0 < self.bytes <= MAX_PACKAGE_BYTES:
            raise ValueError('Package exceeds the optional sync download limit')
        if not isinstance(self.sha256, str) or not re.fullmatch('[0-9a-f]{64}', self.sha256):
            raise ValueError('Invalid package SHA-256')

    @classmethod
    def from_dict(cls, value):
        if not isinstance(value, dict) or set(value) != {'page_url', 'file_id', 'bytes', 'sha256'}:
            raise ValueError('Invalid mod sync offer fields')
        return cls(**value)

    def to_dict(self):
        return asdict(self)

    @property
    def cache_name(self):
        return self.sha256 + '.zip'
