"""Run the real Python HTTPS client against the loopback Go bridge fixture.

Usage: python tests/mod_sync_interop.py PATH_TO_MODSYNC_DEMO
No game files, public server, CDN downloads or real account credentials.
"""
from paths import ROOT
import hashlib
import hmac
import json
import secrets
import ssl
import subprocess
import sys
import time
import threading
import urllib.error
import urllib.request

from mod_sync.models import Offer
from mod_sync.protocol import RyuNet
from mod_sync.transport import NoRedirect, Transport


def main(binary):
    process = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               text=True, encoding='utf-8')
    watchdog = threading.Timer(30, process.kill)
    watchdog.start()
    try:
        info = json.loads(process.stdout.readline())
        context = ssl.create_default_context(cadata=info['certificate'])
        transport = Transport(timeout=5)
        transport.private = urllib.request.build_opener(
            urllib.request.ProxyHandler({}), NoRedirect(), urllib.request.HTTPSHandler(context=context))

        def proof(method, target):
            ts, nonce = str(int(time.time())), secrets.token_hex(16)
            message = f'{ts}\n{nonce}\n{method}\n{target}'.encode()
            digest = hmac.new(info['fixture_key'].encode(), message, hashlib.sha256).hexdigest()
            return f'1.{ts}.{nonce}.{digest}'

        def command(**values):
            process.stdin.write(json.dumps(values) + '\n')
            process.stdin.flush()
            return json.loads(process.stdout.readline())['hold']

        us = RyuNet(info['origin'], 'us', proof, transport)
        eu = RyuNet(info['origin'], 'eu', proof, transport)
        assert us.capabilities()
        a = Offer('https://gamebanana.com/mods/123456', 1, 100, 'a' * 64)
        b = Offer(a.page_url, 2, 110, 'b' * 64)
        preparing = dict(phase='preparing', session='', generation='', loaded=[])
        us.exchange([a], preparing)
        snapshot = eu.exchange([b], preparing)
        assert len(snapshot.offers) == 2
        assert command(countdown=True)
        # Different region's downloaded hash does NOT prove the entire union loaded.
        partial = dict(phase='ready', session=snapshot.session,
                       generation=snapshot.generation, loaded=[b.sha256])
        assert eu.exchange([b], partial).hold
        fallback = dict(partial, phase='fallback', loaded=[])
        us.exchange([a], fallback)
        assert not eu.exchange([b], fallback).hold
        assert not command()  # disabled third member never contacted sync
        assert not eu.exchange([b], preparing).hold  # cannot reacquire this round

        command(round='2', countdown=False)
        us.exchange([a], preparing)
        eu.exchange([b], preparing)
        assert command(countdown=True)
        assert not command(offset_seconds=21)  # deadline, no sleep or wall-clock mutation
        assert not command()

        command(round='3', countdown=False)
        us.exchange([a], preparing)
        assert command(countdown=True)
        assert not command(members=['eu', 'disabled'])  # disconnected advertiser
        assert us.exchange([a], preparing) is None  # authenticated but no lobby

        bad = RyuNet(info['origin'], 'eu', lambda *_: '1.0.bad.bad', transport)
        try:
            bad.exchange([], preparing)
            raise AssertionError('invalid proof accepted')
        except urllib.error.HTTPError as error:
            assert error.code == 401
        # Replay the identical valid proof (body and target unchanged).
        held = []
        def replay(method, target):
            if not held: held.append(proof(method, target))
            return held[0]
        repeated = RyuNet(info['origin'], 'eu', replay, transport)
        repeated.exchange([], preparing)
        try:
            repeated.exchange([], preparing)
            raise AssertionError('replayed proof accepted')
        except urllib.error.HTTPError as error:
            assert error.code == 401
        assert not command(enabled=False)
        print('PASS: HTTPS Python/Go interoperability, mixed-region fallback, deadline, '
              'disconnect, opt-out, invalid/replayed proof, administrator disable')
    finally:
        process.stdin.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        process.stdout.close()
        watchdog.cancel()


if __name__ == '__main__':
    main(sys.argv[1])
