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
