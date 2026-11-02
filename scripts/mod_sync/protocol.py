"""RyuNET adapter contract. Membership and countdown policy are server-owned.

SPDX-License-Identifier: GPL-2.0-or-later
"""
from dataclasses import dataclass
import hashlib
import json
import time
import urllib.error
import urllib.parse

from .models import MAX_OFFERS, Offer
from .transport import Transport


@dataclass(frozen=True)
class Snapshot:
    session: str
    generation: str
    offers: tuple[Offer, ...]
    hold: bool

    @classmethod
    def parse(cls, value, now=None):
        if not isinstance(value, dict) or value.get('protocol') != 2:
            raise ValueError('Unsupported mod sync response')
        now = time.time() if now is None else now
        expiry = value.get('expires_at')
        if type(expiry) not in (float, int) or not now < expiry <= now + 60:
            raise ValueError('Stale or invalid mod sync snapshot')
        for key in ('session', 'generation'):
            if not isinstance(value.get(key), str) or not 1 <= len(value[key]) <= 128:
                raise ValueError('Invalid lobby identity')
        values = value.get('offers')
        if not isinstance(values, list) or len(values) > MAX_OFFERS or type(value.get('hold')) is not bool:
            raise ValueError('Invalid mod sync roster')
        offers = tuple(Offer.from_dict(v) for v in values)
        if len({v.sha256 for v in offers}) != len(offers):
            raise ValueError('Duplicate offers in lobby snapshot')
        return cls(value['session'], value['generation'], offers, value['hold'])


class RyuNet:
    """Uses OCTR's current proof callback, but never falls back to cleartext HTTP.

    The v2 server route is NEW. Upstream v0.7.5 does not implement it. An unsupported
    server leaves ordinary play unchanged; see docs/mod-sync-integration.txt.
    """
    def __init__(self, origin, nickname, proof, transport=None, *, clock_offset=0):
        parsed = urllib.parse.urlsplit(origin)
        if (parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password or
                parsed.path not in ('', '/') or parsed.query or parsed.fragment):
            raise ValueError('Expected an HTTPS server origin')
        self.origin = origin.rstrip('/')
        self.nickname = nickname
        self.proof = proof
        self.transport = transport or Transport()
        self.clock_offset = clock_offset

    def capabilities(self):
        target = '/ryunet/modsync/v2/capabilities'
        reply = self.transport.json(self.origin + target, private=True)
        return (isinstance(reply, dict) and reply.get('protocol') == 2 and
                reply.get('optional_fallback') is True and reply.get('max_hold_seconds') == 20)

    def exchange(self, offers, report):
        if len(offers) > MAX_OFFERS:
            raise ValueError('Too many advertised packages')
        body = json.dumps({'protocol': 2, 'offers': [x.to_dict() for x in offers], 'report': report},
                          sort_keys=True, separators=(',', ':')).encode('utf-8')
        # Bind the POST body to the existing method/target HMAC without changing its format.
        target = '/ryunet/modsync/v2/exchange?' + urllib.parse.urlencode({
            'nick': self.nickname, 'body_sha256': hashlib.sha256(body).hexdigest()})
        try:
            reply = self.transport.json(self.origin + target, private=True, body=body,
                                        headers={'X-RyuNET-Proof': self.proof('POST', target)})
        except urllib.error.HTTPError as error:
            if error.code == 409:  # authenticated, but not currently in a lobby
                return None
            raise
        return Snapshot.parse(reply, time.time() + self.clock_offset)
