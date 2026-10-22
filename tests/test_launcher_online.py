"""The Linux launcher's online helpers: Discord presence over a fake Discord socket, the Discord
authorization with a fake browser, and the server setting's split. No network."""
import json
import os
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
import urllib.request

from paths import ROOT

sys.path.insert(0, str(ROOT / 'launcher'))
sys.path.insert(0, str(ROOT))
import discord_ipc  # noqa: E402
import discord_oauth  # noqa: E402
import ryunet_http  # noqa: E402


class ServerTests(unittest.TestCase):
    def test_split_and_validate(self):
        self.assertEqual(ryunet_http.split_server('octr.ryu7w7.xyz'), ('octr.ryu7w7.xyz', 8080))
        self.assertEqual(ryunet_http.split_server('10.0.0.2:8093'), ('10.0.0.2', 8093))
        self.assertEqual(ryunet_http.split_server(''), ('octr.ryu7w7.xyz', 8080))
        self.assertTrue(ryunet_http.valid_server('octr.ryu7w7.xyz:8080'))
        self.assertFalse(ryunet_http.valid_server('evil.example/path'))
        self.assertFalse(ryunet_http.valid_server(''))


class KeyBackupTests(unittest.TestCase):
    """ryunet_online.key outside the game folder (%APPDATA%, or ~/.local/share on Linux): a clean
    install on the same PC keeps the nicknames."""

    def test_new_folder_takes_the_backup(self):
        import run
        with tempfile.TemporaryDirectory() as base:
            saved = {k: os.environ.get(k) for k in ('APPDATA', 'XDG_DATA_HOME')}
            os.environ['APPDATA'] = os.environ['XDG_DATA_HOME'] = os.path.join(base, 'data')
            try:
                old, new = os.path.join(base, 'v05'), os.path.join(base, 'v06')
                os.makedirs(old)
                os.makedirs(new)
                key = 'ab' * 32
                with open(os.path.join(old, 'ryunet_online.key'), 'w') as f:
                    f.write(key + '\n')
                # An older folder's key is copied out the first time it runs...
                self.assertEqual(run.online_key(old), key)
                backup = os.path.join(base, 'data', 'CTR-NitroFueled-PC', 'ryunet_online.key')
                with open(backup) as f:
                    self.assertEqual(f.read().strip(), key)
                # ...and a clean install gets it back.
                self.assertEqual(run.online_key(new), key)
                with open(os.path.join(new, 'ryunet_online.key')) as f:
                    self.assertEqual(f.read().strip(), key)
            finally:
                for k, v in saved.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v


class KeySwitchTests(unittest.TestCase):
    """Two folders on one PC with different keys: the nickname belongs to the other folder's key,
    and the nickname check makes this folder use it."""

    def test_taken_nickname_of_another_folder(self):
        import hashlib
        import hmac
        import http.server
        import run
        with tempfile.TemporaryDirectory() as base:
            saved = {k: os.environ.get(k) for k in ('APPDATA', 'XDG_DATA_HOME')}
            os.environ['APPDATA'] = os.environ['XDG_DATA_HOME'] = os.path.join(base, 'data')
            self.addCleanup(lambda: [os.environ.pop(k, None) if v is None else os.environ.__setitem__(k, v)
                                     for k, v in saved.items()])
            first, second = os.path.join(base, 'first'), os.path.join(base, 'second')
            os.makedirs(first)
            os.makedirs(second)
            owner = run.online_key(first)  # the first folder owns the nickname
            with open(os.path.join(second, 'ryunet_online.key'), 'w') as f:
                f.write('cd' * 32 + '\n')  # the second one has a key of its own
            run.online_key(second)
            self.assertEqual(set(run.known_keys()), {owner, 'cd' * 32})
            owner_hash = run.key_hash(run.server_key(owner, '127.0.0.1'))

            class Handler(http.server.BaseHTTPRequestHandler):
                def do_GET(self):
                    _, ts, nonce, mac = self.headers.get('X-RyuNET-Proof', '1.0.0.0').split('.')
                    want = hmac.new(owner_hash.encode(), f'{ts}\n{nonce}\nGET\n{self.path}'.encode(),
                                    hashlib.sha256).hexdigest()
                    body = json.dumps({'status': 'yours' if mac == want else 'taken'}).encode()
                    self.send_response(200)
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)

                def log_message(self, *args):
                    pass

            server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
            threading.Thread(target=server.serve_forever, daemon=True).start()
            self.addCleanup(server.server_close)
            self.addCleanup(server.shutdown)
            address = f'127.0.0.1:{server.server_address[1]}'
            answer = ryunet_http.nick_status(second, address, 'Tester')
            self.assertEqual(answer, {'status': 'yours', 'key_switched': True})
            self.assertEqual(run.online_key(second), owner)
            # From then on the folder's own key answers.
            self.assertEqual(ryunet_http.nick_status(second, address, 'Tester'), {'status': 'yours'})


class ClockTests(unittest.TestCase):
    """A PC whose clock is an hour off: the server (a local fake with nick_proof.go's 5-minute
    window) refuses the first signature, and its Date sets the time of the second."""

    def setUp(self):
        import email.utils
        import http.server
        import run
        self.run = run
        run.SERVER_CLOCK_OFFSET = 0
        self.addCleanup(setattr, run, 'SERVER_CLOCK_OFFSET', 0)
        skew = self.skew = 3600  # the server's clock is an hour ahead of this PC's
        seen = self.seen = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                now = time.time() + skew
                ts = int(self.headers.get('X-RyuNET-Proof', '1.0').split('.')[1])
                seen.append(ts)
                ok = abs(ts - now) <= 300
                body = json.dumps({'status': 'yours'} if ok else {'error': 'out of time'}).encode()
                self.send_response(200 if ok else 403)
                self.send_header('Date', email.utils.formatdate(now, usegmt=True))
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def date_time_string(self, timestamp=None):
                return email.utils.formatdate(time.time() + skew, usegmt=True)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.data = tempfile.TemporaryDirectory()
        self.addCleanup(self.data.cleanup)

    def test_signature_follows_the_server_clock(self):
        server = f'127.0.0.1:{self.server.server_address[1]}'
        answer = ryunet_http.request(self.data.name, server, '/ryunet/nick?name=Tester')
        self.assertEqual(answer, {'status': 'yours'})
        self.assertEqual(len(self.seen), 2)  # refused once, then signed with the server's time
        self.assertAlmostEqual(ryunet_http.clock_offset(), self.skew, delta=3)
        # From then on the first signature is good.
        self.assertEqual(ryunet_http.request(self.data.name, server, '/ryunet/nick?name=Tester'), {'status': 'yours'})
        self.assertEqual(len(self.seen), 3)

    def test_small_differences_are_ignored(self):
        class Reply:
            headers = {'Date': __import__('email.utils').utils.formatdate(time.time() + 2, usegmt=True)}
        self.run.note_server_clock(Reply)
        self.assertEqual(self.run.SERVER_CLOCK_OFFSET, 0)
        Reply.headers = {'Date': 'garbage'}
        self.run.note_server_clock(Reply)
        self.assertEqual(self.run.SERVER_CLOCK_OFFSET, 0)


class OAuthTests(unittest.TestCase):
    def browser(self, query_for):
        """A fake browser: follows the authorization URL straight back to the redirect."""
        def open_url(url):
            params = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
            redirect, state = params['redirect_uri'][0], params['state'][0]

            def go():
                time.sleep(0.2)
                try:
                    urllib.request.urlopen(f'{redirect}?{query_for(state)}', timeout=5).read()
                except OSError:
                    pass
            threading.Thread(target=go, daemon=True).start()
        return open_url

    def test_code_comes_back(self):
        result = discord_oauth.authorize('123', self.browser(lambda s: f'code=abc&state={s}'), timeout=10)
        self.assertTrue(result['ok'], result)
        self.assertEqual(result['code'], 'abc')
        self.assertTrue(result['redirect_uri'].startswith('http://127.0.0.1:5313'))

    def test_denied_and_foreign_state(self):
        result = discord_oauth.authorize('123', self.browser(lambda s: f'error=access_denied&state={s}'), timeout=10)
        self.assertEqual(result['error'], 'denied')
        # Another program's redirect is ignored: the wait ends without a code.
        result = discord_oauth.authorize('123', self.browser(lambda s: 'code=stolen&state=other'), timeout=2)
        self.assertEqual(result['error'], 'timeout')


@unittest.skipUnless(hasattr(socket, 'AF_UNIX') and os.name == 'posix', 'Discord IPC uses a Unix socket')
class PresenceTests(unittest.TestCase):
    def test_handshake_activity_join_and_requests(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'discord-ipc-0')
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            server.bind(path)
            server.listen(1)
            got = []

            def read_frame(conn):
                head = b''
                while len(head) < 8:
                    head += conn.recv(8 - len(head))
                op, length = struct.unpack('<II', head)
                body = b''
                while len(body) < length:
                    body += conn.recv(length - len(body))
                return op, json.loads(body)

            def send(conn, op, payload):
                data = json.dumps(payload).encode()
                conn.sendall(struct.pack('<II', op, len(data)) + data)

            def fake_discord():
                conn, _ = server.accept()
                op, hello = read_frame(conn)
                got.append(('handshake', hello['client_id']))
                send(conn, 1, {'cmd': 'DISPATCH', 'evt': 'READY', 'data': {}})
                while True:
                    try:
                        op, frame = read_frame(conn)
                    except (OSError, struct.error, ValueError):
                        return
                    got.append((frame.get('cmd'), frame.get('evt'), frame.get('args')))
                    send(conn, 1, {'cmd': frame['cmd'], 'nonce': frame['nonce'], 'data': {}})
                    if frame.get('cmd') == 'SET_ACTIVITY' and frame['args']['activity']:
                        # A player accepts an invite, another asks to join.
                        send(conn, 1, {'cmd': 'DISPATCH', 'evt': 'ACTIVITY_JOIN', 'data': {'secret': 'ctr1|s|Host'}})
                        send(conn, 1, {'cmd': 'DISPATCH', 'evt': 'ACTIVITY_JOIN_REQUEST',
                                       'data': {'user': {'id': '42'}}})

            threading.Thread(target=fake_discord, daemon=True).start()
            os.environ['CTR_DISCORD_SOCKET'] = path
            try:
                presence = discord_ipc.Presence()
                activity = discord_ipc.Activity('Playing online', state='Nickname: Me', start=100,
                                                party_id='ctr:s:me', party_size=1, party_max=8,
                                                join_secret='ctr1|s|Me')
                presence.update(True, '999', activity)
                secret = None
                for _ in range(100):
                    # take_join hands the secret out once: keep it while the invite reply arrives.
                    secret = secret or presence.take_join()
                    if secret and any(g[0] == 'SEND_ACTIVITY_JOIN_INVITE' for g in got):
                        break
                    time.sleep(0.05)
                self.assertEqual(secret, 'ctr1|s|Host')
                self.assertEqual(presence.status()[0], discord_ipc.CONNECTED)
                presence.shutdown()
            finally:
                del os.environ['CTR_DISCORD_SOCKET']
                server.close()
            self.assertEqual(got[0], ('handshake', '999'))
            subscribed = {g[1] for g in got if g[0] == 'SUBSCRIBE'}
            self.assertEqual(subscribed, {'ACTIVITY_JOIN', 'ACTIVITY_JOIN_REQUEST'})
            sent = next(g[2] for g in got if g[0] == 'SET_ACTIVITY')
            self.assertEqual(sent['activity']['party'], {'id': 'ctr:s:me', 'size': [1, 8]})
            self.assertEqual(sent['activity']['secrets'], {'join': 'ctr1|s|Me'})
            self.assertIn(('SEND_ACTIVITY_JOIN_INVITE', None, {'user_id': '42'}), got)

    def test_no_discord(self):
        os.environ['CTR_DISCORD_SOCKET'] = '/nonexistent/discord-ipc-0'
        try:
            presence = discord_ipc.Presence()
            presence.update(True, '999', None)
            for _ in range(40):
                if presence.status()[0] == discord_ipc.NO_DISCORD:
                    break
                time.sleep(0.05)
            self.assertEqual(presence.status()[0], discord_ipc.NO_DISCORD)
            presence.shutdown()
        finally:
            del os.environ['CTR_DISCORD_SOCKET']


if __name__ == '__main__':
    unittest.main()
