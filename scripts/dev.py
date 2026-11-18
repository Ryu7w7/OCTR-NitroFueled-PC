#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Dev launch against your own RyuNET server: start it, play, leave nothing behind.

Linux and Windows (from an MSYS2 shell, as run.sh): the same paths and the same ryunet server
layout. The server goes in the environment (run.sh -> online_config), never in bbport.ini, so the
packaged game keeps the server it was set to. The data folder's auth-key patch is removed on exit,
Ctrl-C included: left behind, it makes production auth fail too (issue #0014).

    python scripts/dev.py                 # 127.0.0.1:8080, the server you already run
    python scripts/dev.py --software      # extra arguments go to run.sh
    RYUNET_DIR=...                        # your server checkout: dev.py starts its bin/server
    BB_ONLINE_SERVER=... BB_ONLINE_HTTP_PORT=... BB_DATA_DIR=... BB_AUTH_KEY=...
    BB_NO_SERVER=1                        # a server is already running (or elsewhere)

BB_DATA_DIR means what it means to run.sh (a data folder, "." = this checkout). The installed
game's folder is used when it is the one exported; nothing here guesses a home directory.
"""
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

PORT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_ROOT / 'scripts'))
from online_key_local import restore  # noqa: E402


def data_dir() -> Path:
    """run.sh's data folder: BB_DATA_DIR, relative to this checkout, else the checkout itself."""
    raw = Path(os.environ.get('BB_DATA_DIR') or '.')
    return raw if raw.is_absolute() else PORT_ROOT / raw


def listening(host: str, port: int) -> bool:
    try:
        with socket.create_connection((host, port), timeout=0.25):
            return True
    except OSError:
        return False


def server_binary(ryunet: Path) -> Path:
    return ryunet / 'bin' / ('server.exe' if os.name == 'nt' else 'server')


def main() -> int:
    ryunet = os.environ.get('RYUNET_DIR')
    host = os.environ.get('BB_ONLINE_SERVER') or '127.0.0.1'
    port = int(os.environ.get('BB_ONLINE_HTTP_PORT') or 8080)
    data = data_dir()
    env = dict(os.environ, BB_DATA_DIR=str(data), BB_ONLINE='1',
               BB_ONLINE_SERVER=host, BB_ONLINE_HTTP_PORT=str(port))

    server, log = None, None
    try:
        if os.environ.get('BB_NO_SERVER') != '1' and not listening(host, port):
            if not ryunet:
                print(f'dev: nothing listens on {host}:{port}; start your server, or set '
                      f'RYUNET_DIR to its folder so dev.py starts bin/server from it', file=sys.stderr)
                return 1
            binary = server_binary(Path(ryunet).resolve())
            if not binary.is_file():
                print(f'dev: no server binary at {binary}; set RYUNET_DIR to its folder', file=sys.stderr)
                return 1
            (data / 'logs').mkdir(parents=True, exist_ok=True)
            log = open(data / 'logs' / 'dev-server.log', 'wb')
            print(f'dev: starting {binary} (log: {data / "logs" / "dev-server.log"})', flush=True)
            server = subprocess.Popen([str(binary)], cwd=binary.parent.parent,
                                      stdout=log, stderr=subprocess.STDOUT)
            for _ in range(40):
                if listening(host, port):
                    break
                if server.poll() is not None:
                    print(f'dev: the server stopped; see {data / "logs" / "dev-server.log"}',
                          file=sys.stderr)
                    return 1
                time.sleep(0.25)
            else:
                print(f'dev: {host}:{port} did not open; see {data / "logs" / "dev-server.log"}',
                      file=sys.stderr)
                return 1
        return subprocess.call(['bash', str(PORT_ROOT / 'run.sh'), *sys.argv[1:]], env=env)
    finally:
        restore(data)
        if server:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
        if log:
            log.close()


if __name__ == '__main__':
    sys.exit(main())
