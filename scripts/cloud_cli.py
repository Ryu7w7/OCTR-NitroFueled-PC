# SPDX-License-Identifier: GPL-2.0-or-later
"""The launchers' cloud save actions (both use it: the Windows launcher runs it, the Linux one
imports it). One JSON object is printed per run.

    cloud_cli.py list    --data <folder> --config <bbport.ini>
    cloud_cli.py restore --data <folder> --config <bbport.ini> --id <version>
    cloud_cli.py upload  --data <folder> --config <bbport.ini> --title CUSA13795

Requests are signed with this installation's key, as run.py signs them (cloud_sync.py has the
details). A restore keeps the save it replaces in <user>/cloud/backup-<title>.zip.
"""
import argparse
import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for p in (ROOT, ROOT / 'scripts'):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

import run  # noqa: E402
import cloud_sync  # noqa: E402

TITLES = ('CUSA13795', 'CUSA14876')


def settings(config):
    values = {}
    try:
        for line in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            k, sep, v = line.partition('=')
            if sep and not k.startswith('#'):
                values[k.strip()] = v.strip()
    except OSError:
        pass
    server = values.get('online_server') or 'octr.ryu7w7.xyz'
    host, port = server, values.get('online_http_port', '8080')
    if ':' in server and not server.startswith('['):
        h, _, p = server.partition(':')
        if p.isdigit():
            host, port = h, p
    return host, port, values.get('online_name', values.get('user_name', '')).strip()


class Account:
    def __init__(self, data, config, user=None):
        self.data = Path(data)
        self.host, self.port, self.name = settings(config)
        key = run.online_key(self.data)
        if not key or not run.nick_ok(self.name):
            raise SystemExit(json.dumps({'error': 'no_account'}))
        self.khash = run.key_hash(run.server_key(key, self.host))
        self.progress = Path(user) if user else self.data / 'user'
        run.server_settings(self.host, self.port)  # the server's clock, before signing

    def call(self, method, target, body=None, timeout=20):
        answer, _ = run.ryunet_json(self.host, self.port, target, timeout, method=method, body=body,
                                    headers=lambda m, t: {'X-RyuNET-Proof': run.proof_header(self.khash, m, t)})
        return answer

    def fetch(self, target, timeout=60):
        """A signed GET that answers bytes (a save's zip): by HTTPS on the name, else the port."""
        urls = []
        if not self.host.replace('.', '').isdigit():
            urls.append(f'https://{self.host}{target}')
        urls.append(f'http://{self.host}:{self.port}{target}')
        last = OSError('no answer')
        for url in urls:
            req = urllib.request.Request(url, headers={'X-RyuNET-Proof': run.proof_header(self.khash, 'GET', target),
                                                      'User-Agent': 'SacredFire launcher'})
            try:
                with urllib.request.urlopen(req, timeout=timeout) as reply:
                    if 'zip' not in (reply.headers.get('Content-Type') or ''):
                        last = OSError(f'{url}: not a save')
                        continue
                    return reply.read(cloud_sync.SAVE_MAX + 1)
            except (OSError, urllib.error.HTTPError) as error:
                last = error
        raise last


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('action', choices=('list', 'restore', 'upload'))
    ap.add_argument('--data', required=True)
    ap.add_argument('--config', required=True)
    ap.add_argument('--id', type=int)
    ap.add_argument('--title', default='')
    ap.add_argument('--user', default='')
    args = ap.parse_args(argv)
    try:
        acc = Account(args.data, args.config, args.user or None)
        if args.action == 'list':
            answer = acc.call('GET', '/ryunet/saves/list?' + urllib.parse.urlencode({'name': acc.name}))
            out = {'nick': acc.name, 'saves': answer.get('saves', []) if isinstance(answer, dict) else []}
        elif args.action == 'restore':
            listed = acc.call('GET', '/ryunet/saves/list?' + urllib.parse.urlencode({'name': acc.name}))
            version = next((s for s in listed.get('saves', []) if s.get('id') == args.id), None)
            if not version or version.get('title') not in TITLES:
                raise ValueError('no such version')
            data = acc.fetch('/ryunet/saves/download?' + urllib.parse.urlencode({'name': acc.name, 'id': args.id}))
            cloud_sync.restore_save(data, acc.progress, version['title'])
            out = {'restored': args.id, 'title': version['title'],
                   'backup': str(acc.progress / 'cloud' / f'backup-{version["title"]}.zip')}
        else:
            if args.title not in TITLES:
                raise ValueError('title')
            vid = cloud_sync.upload_save(acc.call, acc.name, acc.progress, args.title, say=lambda *_: None)
            out = {'uploaded': vid}
    except SystemExit:
        raise
    except (OSError, ValueError, zipfile_error()) as error:
        out = {'error': str(error) or type(error).__name__}
    print(json.dumps(out))
    return 0 if 'error' not in out else 1


def zipfile_error():
    import zipfile
    return zipfile.BadZipFile


if __name__ == '__main__':
    sys.exit(main())
