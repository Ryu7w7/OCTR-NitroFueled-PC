"""Bounded HTTPS, explicit redirect policy, no credentials on CDN requests.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import hashlib
import json
from pathlib import Path
import re
import threading
import time
import urllib.parse
import urllib.request

USER_AGENT = 'OCTR-Optional-ModSync/0.1 (+https://github.com/Ryu7w7/OCTR-NitroFueled-PC)'


class Cancelled(Exception):
    pass


def gamebanana_url(url):
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != 'https' or parsed.username or parsed.password or
            parsed.port not in (None, 443) or parsed.fragment or
            not re.fullmatch(r'(?:gamebanana\.com|files\.gamebanana\.com|filecache[0-9]+\.gamebanana\.com)', parsed.hostname or '')):
        raise ValueError('Download must remain on an approved GameBanana HTTPS host')


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        return None


class GameBananaRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        gamebanana_url(newurl)
        return super().redirect_request(request, fp, code, msg, headers, newurl)


class Transport:
    def __init__(self, *, timeout=10):
        self.timeout = timeout
        # Separate openers: account proofs are never forwarded to a CDN redirect.
        self.public = urllib.request.build_opener(GameBananaRedirect())
        self.private = urllib.request.build_opener(NoRedirect())

    def json(self, url, *, body=None, headers=None, private=False, limit=1024**2):
        if private:
            p = urllib.parse.urlsplit(url)
            if p.scheme != 'https' or p.username or p.password or p.fragment:
                raise ValueError('Mod sync server requires HTTPS')
        else:
            gamebanana_url(url)
            if body is not None or headers:
                raise ValueError('Credentials cannot be attached to GameBanana requests')
        request = urllib.request.Request(url, data=body, headers={
            'User-Agent': USER_AGENT, 'Accept': 'application/json',
            **({'Content-Type': 'application/json'} if body is not None else {}), **(headers or {})})
        with (self.private if private else self.public).open(request, timeout=self.timeout) as response:
            if 'application/json' not in response.headers.get('Content-Type', ''):
                raise ValueError('Expected JSON from the mod service')
            raw = response.read(limit + 1)
            if len(raw) > limit:
                raise ValueError('Mod service response exceeds limit')
            return json.loads(raw)

    def download(self, url, destination: Path, expected_bytes: int, cancelled: threading.Event, progress=None):
        gamebanana_url(url)
        request = urllib.request.Request(url, headers={'User-Agent': USER_AGENT, 'Accept-Encoding': 'identity'})
        count = 0
        digest = hashlib.sha256()
        started = time.monotonic()
        with self.public.open(request, timeout=self.timeout) as response, destination.open('xb') as output:
            length = response.headers.get('Content-Length')
            if length is not None and int(length) != expected_bytes:
                raise ValueError('Download length differs from the selected upload')
            while True:
                if cancelled.is_set():
                    raise Cancelled()
                if time.monotonic() - started > 300:
                    raise TimeoutError('Optional download exceeded five minutes')
                chunk = response.read(128 * 1024)
                if not chunk:
                    break
                count += len(chunk)
                if count > expected_bytes:
                    raise ValueError('Download exceeded its advertised length')
                digest.update(chunk)
                output.write(chunk)
                if progress:
                    progress(count, expected_bytes)
        if count != expected_bytes:
            raise ValueError('Incomplete GameBanana download')
        return digest.hexdigest()
