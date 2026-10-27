"""run.py's requests to the RyuNET server behind an ISP filter that answers for the server's name
(Spectrum's Security Shield redirected them to its warning page, so the nickname was never
registered and every login was refused). A local fake plays both: by name the filter, by address
the server. No network."""
import http.server
import json
import sys
import threading
import unittest

from paths import ROOT

sys.path.insert(0, str(ROOT))


class FilteredServerTests(unittest.TestCase):
    def setUp(self):
        import run
        self.run = run
        self.lines = []
        said = run.say
        run.say = self.lines.append
        self.addCleanup(setattr, run, 'say', said)
        self.filter = 'redirect'
        self.seen = []
        test = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def answer(self):
                host = self.headers.get('Host', '').split(':')[0]
                test.seen.append((self.command, host, self.path.split('?')[0],
                                  self.headers.get('X-RyuNET-Proof')))
                if host == 'localhost' and test.filter == 'redirect':
                    self.send_response(302)
                    self.send_header('Location', 'https://block.example/warn.html')
                    self.end_headers()
                    return
                if host == 'localhost' and test.filter == 'page':
                    self.reply(b'<html>blocked</html>', 'text/html')
                    return
                status = 'claimed' if self.path.startswith('/ryunet/nick/claim') else 'free'
                answer = {'theme': 'halloween'} if self.path == '/ryunet/theme' else {'status': status}
                self.reply(json.dumps(answer).encode(), 'application/json')

            def reply(self, body, kind):
                self.send_response(200)
                self.send_header('Content-Type', kind)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            do_GET = do_POST = answer

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.port = self.server.server_address[1]

    def claim_through(self, kind):
        self.filter = kind
        self.assertEqual(self.run.server_settings('localhost', self.port), {'theme': 'halloween'})
        self.run.claim_nick('localhost', self.port, 'Tester', 'ab' * 32)
        self.assertEqual(self.lines, ['Nickname Tester: claimed on localhost'])
        # Each request went by name, then by address; a signature is never sent twice.
        self.assertEqual([(m, h, p) for m, h, p, _ in self.seen],
                         [('GET', 'localhost', '/ryunet/theme'), ('GET', '127.0.0.1', '/ryunet/theme'),
                          ('GET', 'localhost', '/ryunet/nick'), ('GET', '127.0.0.1', '/ryunet/nick'),
                          ('POST', 'localhost', '/ryunet/nick/claim'), ('POST', '127.0.0.1', '/ryunet/nick/claim')])
        proofs = [proof for _, _, _, proof in self.seen if proof]
        self.assertEqual(len(proofs), len(set(proofs)))

    def test_redirect_to_a_warning_page(self):
        self.claim_through('redirect')

    def test_warning_page_served_for_the_name(self):
        self.claim_through('page')

    def test_no_filter_asks_once(self):
        self.filter = 'none'
        self.run.claim_nick('localhost', self.port, 'Tester', 'ab' * 32)
        self.assertEqual([(m, h) for m, h, _, _ in self.seen], [('GET', 'localhost'), ('POST', 'localhost')])


if __name__ == '__main__':
    unittest.main()
