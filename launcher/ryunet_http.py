# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's requests to the RyuNET server's HTTP port, as the Windows launcher makes them
(launcher/windows/launcher.cpp HttpRequest): nickname status, the hosted party's size, server
announcements and Discord account links. Each request carries this installation's key for that
server (X-RyuNET-Key: HMAC-SHA256 of the host, run.py server_key), so the server knows whose
nickname it is. Blocking: call them from a worker thread.
"""
import json
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import run  # online_key, server_key: the same key file and derivation as the game

DEFAULT_SERVER = "octr.ryu7w7.xyz"
DEFAULT_HTTP_PORT = 8080


def split_server(server):
    """(host, port) of the server setting, split as run.py does ("host" or "host:port")."""
    server = (server or DEFAULT_SERVER).strip()
    host, port = server, DEFAULT_HTTP_PORT
    if ":" in server and not server.startswith("["):
        name, _, digits = server.partition(":")
        if digits.isdigit():
            host, port = name, int(digits)
    return host, port


def valid_server(server):
    """A server that may travel in a Discord join secret: a host name or IP with an optional port."""
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:")
    return bool(server) and len(server) <= 64 and set(server) <= allowed


def request(data_dir, server, path, form=None, timeout=3.0, register=False):
    """The answer's JSON object, or None when the server could not be reached or answered
    something else. form: a dict, sent as application/x-www-form-urlencoded (a POST). The request
    is signed with this installation's key (run.proof_header); register also sends the key's hash,
    which a Discord link takes for a new installation."""
    host, port = split_server(server)
    headers = {"User-Agent": "SacredFire launcher"}
    key = run.online_key(Path(data_dir))
    if key:
        khash = run.key_hash(run.server_key(key, host))
        headers["X-RyuNET-Proof"] = run.proof_header(khash, "POST" if form is not None else "GET", path)
        if register:
            headers["X-RyuNET-Key-Hash"] = khash
    body = None
    if form is not None:
        body = urllib.parse.urlencode(form).encode("utf-8")
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    req = urllib.request.Request(f"http://{host}:{port}{path}", data=body, headers=headers,
                                 method="POST" if form is not None else "GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as reply:
            answer = json.loads(reply.read(65536).decode("utf-8", "replace"))
    except urllib.error.HTTPError as error:
        try:
            answer = json.loads(error.read(65536).decode("utf-8", "replace"))
        except (OSError, ValueError):
            return None
    except (OSError, ValueError):
        return None
    return answer if isinstance(answer, dict) else None


def nick_status(data_dir, server, nick):
    """{"status": free|yours|taken|reserved|invalid, "linked", "discord_link", "discord"}, or None."""
    return request(data_dir, server, "/ryunet/nick?name=" + urllib.parse.quote(nick or ""))


def party(data_dir, server, nick):
    """(players, max) of the lobby this nickname hosts, or None when unknown."""
    answer = request(data_dir, server, "/ryunet/party?name=" + urllib.parse.quote(nick))
    try:
        return int(answer["players"]), int(answer["max"])
    except (TypeError, KeyError, ValueError):
        return None


def announcement(data_dir, server):
    """The server's current announcement {"id", "text", "level", "expires_in"}, or None."""
    answer = request(data_dir, server, "/ryunet/announce")
    if not answer or not answer.get("text") or str(answer.get("id", "0")) in ("", "0"):
        return None
    try:
        if int(answer.get("expires_in", 0)) <= 0:
            return None
    except (TypeError, ValueError):
        return None
    return answer


def discord_link(data_dir, server, nick, code, redirect_uri):
    """Trades a Discord authorization code for a link of nick (or, with no nick, recovers the
    account's nickname). {"status": linked|recovered|own_nick|no_nick|other_nick|taken|..., ...}"""
    return request(data_dir, server, "/ryunet/discord/link",
                   {"nick": nick, "code": code, "redirect_uri": redirect_uri}, timeout=25, register=True)


def discord_unlink(data_dir, server, nick):
    answer = request(data_dir, server, "/ryunet/discord/unlink", {"nick": nick}, timeout=10)
    return bool(answer and answer.get("unlinked"))
