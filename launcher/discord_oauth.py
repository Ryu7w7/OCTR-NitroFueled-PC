# SPDX-License-Identifier: GPL-2.0-or-later
"""Discord account authorization for the Linux launcher (OAuth2, scope identify), the browser
half; the same flow as launcher/windows/discord_link.cpp.

authorize() listens on a loopback port, opens Discord's authorization page in the browser and
waits for Discord to redirect the browser back with a code. The code is useless on its own: the
RyuNET server, which holds the application's client secret, trades it for the player's Discord
account (ryunet_http.discord_link). Blocking: call it from a worker thread.
"""
import html
import secrets
import select
import socket
import time
import urllib.parse

# Loopback ports tried in order; each redirect URI http://127.0.0.1:<port>/discord is registered
# in the Discord Developer Portal and allowed by the server.
PORTS = (53134, 53135, 53136)


def _reply(client, status, title, text):
    page = ("<!doctype html><html><head><meta charset=\"utf-8\"><title>" + html.escape(title) +
            "</title></head><body style=\"font-family:sans-serif;background:#1e1f22;color:#eee;"
            "display:flex;align-items:center;justify-content:center;height:100vh;margin:0\">"
            "<div style=\"text-align:center\"><h2>" + html.escape(title) + "</h2><p>" + html.escape(text) +
            "</p></div></body></html>").encode("utf-8")
    head = (f"HTTP/1.1 {status}\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
            f"Connection: close\r\nContent-Length: {len(page)}\r\n\r\n").encode("ascii")
    try:
        client.sendall(head + page)
    except OSError:
        pass


def authorize(client_id, open_url, timeout=180):
    """{"ok", "code", "redirect_uri", "error"}; error is "denied", "timeout", "busy" or
    Discord's own. open_url(url) shows the authorization page (the browser)."""
    listener, port = None, 0
    for p in PORTS:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.bind(("127.0.0.1", p))
            s.listen(4)
        except OSError:
            s.close()
            continue
        listener, port = s, p
        break
    if listener is None:
        return {"ok": False, "error": "busy"}
    state = secrets.token_hex(16)
    redirect_uri = f"http://127.0.0.1:{port}/discord"
    url = ("https://discord.com/oauth2/authorize?" +
           urllib.parse.urlencode({"client_id": client_id, "response_type": "code", "scope": "identify",
                                   "prompt": "none", "redirect_uri": redirect_uri, "state": state}))
    result = {"ok": False, "error": "timeout", "redirect_uri": redirect_uri}
    try:
        open_url(url)
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            ready, _, _ = select.select([listener], [], [], left)
            if not ready:
                break
            client, _addr = listener.accept()
            client.settimeout(5)
            request = b""
            try:
                while b"\r\n\r\n" not in request and len(request) < 16384:
                    chunk = client.recv(2048)
                    if not chunk:
                        break
                    request += chunk
            except OSError:
                pass
            line = request.split(b"\r\n", 1)[0].decode("latin-1")  # GET /discord?code=..&state=.. HTTP/1.1
            parts = line.split(" ")
            target = parts[1] if len(parts) >= 2 and parts[0] == "GET" else ""
            parsed = urllib.parse.urlsplit(target)
            query = urllib.parse.parse_qs(parsed.query)
            if parsed.path != "/discord":
                _reply(client, "404 Not Found", "404", "")
                client.close()
                continue
            if query.get("state", [""])[0] != state:
                # Not this launcher's request (an old tab, another program): keep waiting.
                _reply(client, "400 Bad Request", "Invalid request", "This authorization is not the launcher's.")
                client.close()
                continue
            error = query.get("error", [""])[0]
            code = query.get("code", [""])[0]
            if error or not code:
                result = {"ok": False, "error": "denied" if error in ("", "access_denied") else error,
                          "redirect_uri": redirect_uri}
                _reply(client, "200 OK", "Authorization cancelled", "You can close this tab and go back to the launcher.")
            else:
                result = {"ok": True, "code": code, "redirect_uri": redirect_uri, "error": ""}
                _reply(client, "200 OK", "Done",
                       "Discord account authorized. You can close this tab and go back to the launcher.")
            client.close()
            break
    finally:
        listener.close()
    return result
