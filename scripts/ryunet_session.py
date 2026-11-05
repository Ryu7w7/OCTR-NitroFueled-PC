"""Reuse direct HTTP connections only for one launch's sequential RyuNET setup."""
from contextlib import contextmanager
import http.client
import io
import sys
import urllib.error
import urllib.parse
import urllib.request


class Session:
    def __init__(self):
        self.connections = {}

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        for connection in self.connections.values():
            connection.close()
        self.connections.clear()

    def discard(self, key):
        connection = self.connections.pop(key, None)
        if connection is not None:
            connection.close()

    @contextmanager
    def open(self, request, timeout, opener):
        url = urllib.parse.urlsplit(request.full_url)
        # Keep urllib's system/environment proxy handling intact on Windows/Linux.
        if urllib.request.getproxies() or url.username is not None:
            with opener.open(request, timeout=timeout) as response:
                yield response
            return
        key = (url.scheme, url.hostname, url.port)
        connection = self.connections.get(key)
        if connection is None:
            kind = http.client.HTTPSConnection if url.scheme == 'https' else http.client.HTTPConnection
            # HTTPSConnection uses certificate and hostname verification by default.
            connection = self.connections[key] = kind(url.hostname, url.port, timeout=timeout)
        connection.timeout = timeout
        if connection.sock is not None:
            connection.sock.settimeout(timeout)
        headers = dict(request.header_items())
        headers.setdefault('User-agent', f'Python-urllib/{sys.version_info.major}.{sys.version_info.minor}')
        response = None
        try:
            connection.request(request.get_method(), request.selector, body=request.data, headers=headers)
            response = connection.getresponse()
            if response.status >= 300:
                raise urllib.error.HTTPError(request.full_url, response.status, response.reason,
                                             response.headers, io.BytesIO(response.read(1 << 16)))
            yield response
        except http.client.HTTPException as error:
            self.discard(key)
            raise OSError(str(error)) from error
        except BaseException:
            self.discard(key)
            raise
        finally:
            if response is not None:
                # A partial/oversized response cannot be reused for the next request.
                if response.will_close or not response.isclosed():
                    self.discard(key)
                response.close()
