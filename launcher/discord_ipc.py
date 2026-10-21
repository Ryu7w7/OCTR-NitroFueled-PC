# SPDX-License-Identifier: GPL-2.0-or-later
"""Discord Rich Presence for the Linux launcher, over Discord's local IPC socket
(discord-ipc-N in the runtime directory): no SDK. The same protocol and behaviour as the
Windows launcher's launcher/windows/discord_presence.cpp.

A background thread owns the socket, so a missing or slow Discord never stalls the window; it
retries every few seconds and reconnects when Discord restarts.

Invites: an activity with a party and a join secret gets Discord's "Join" button and the chat's
"+ > Invite to play". Whoever accepts receives the secret in THEIR launcher, through take_join().

Frames: u32 opcode | u32 length (little endian) | UTF-8 JSON.
  op 0 HANDSHAKE {"v":1,"client_id":"..."} -> op 1 DISPATCH "READY", or op 2 CLOSE on a bad id
  op 1 FRAME     {"cmd":"SET_ACTIVITY","args":{"pid":..,"activity":{..}|null},"nonce":".."}
                 {"cmd":"SUBSCRIBE","evt":"ACTIVITY_JOIN","nonce":".."}
  op 2 CLOSE, op 3 PING (answered with op 4 PONG, same payload)
Closing the socket makes Discord drop the presence of that process.
"""
import json
import os
import select
import socket
import struct
import threading
import time
from pathlib import Path

OP_HANDSHAKE, OP_FRAME, OP_CLOSE, OP_PING, OP_PONG = range(5)
RETRY_SECONDS = 15
POLL_SECONDS = 0.2
REPLY_TIMEOUT = 3.0
MAX_FRAME = 64 * 1024

OFF, NO_APP_ID, NO_DISCORD, CONNECTED, ERROR = "off", "no_app_id", "no_discord", "connected", "error"


def socket_paths():
    """Where Discord's socket can be: the runtime directory (or a temporary one), and the
    places the Flatpak and Snap packages of Discord (and Vesktop) put it."""
    if os.environ.get("CTR_DISCORD_SOCKET"):  # tests point it at a fake Discord
        return [os.environ["CTR_DISCORD_SOCKET"]]
    bases = []
    for var in ("XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"):
        if os.environ.get(var):
            bases.append(Path(os.environ[var]))
    bases.append(Path(f"/run/user/{os.getuid()}"))
    bases.append(Path("/tmp"))
    subdirs = ["", "app/com.discordapp.Discord", "app/com.discordapp.DiscordCanary",
               "app/dev.vencord.Vesktop", ".flatpak/com.discordapp.Discord/xdg-run",
               ".flatpak/dev.vencord.Vesktop/xdg-run", "snap.discord", "snap.discord-canary"]
    paths, seen = [], set()
    for base in bases:
        for sub in subdirs:
            for i in range(10):
                path = str(base / sub / f"discord-ipc-{i}")
                if path not in seen:
                    seen.add(path)
                    paths.append(path)
    return paths


class Activity:
    """What the presence shows. Joinable: party_id, party_size, party_max and join_secret."""

    def __init__(self, details, state="", start=0, large_image="", large_text="",
                 party_id="", party_size=0, party_max=0, join_secret=""):
        self.details, self.state, self.start = details, state, start
        self.large_image, self.large_text = large_image, large_text
        self.party_id, self.party_size, self.party_max = party_id, party_size, party_max
        self.join_secret = join_secret

    def key(self):
        return (self.details, self.state, self.start, self.large_image, self.large_text,
                self.party_id, self.party_size, self.party_max, self.join_secret)

    def to_json(self):
        a = {"details": self.details}
        if self.state:
            a["state"] = self.state
        if self.start > 0:
            a["timestamps"] = {"start": int(self.start)}
        if self.large_image:
            a["assets"] = {"large_image": self.large_image}
            if self.large_text:
                a["assets"]["large_text"] = self.large_text
        if self.party_id and self.party_size > 0 and self.party_max >= self.party_size:
            a["party"] = {"id": self.party_id, "size": [self.party_size, self.party_max]}
            if self.join_secret:
                a["secrets"] = {"join": self.join_secret}
        return a


class _Connection:
    def __init__(self):
        self.sock = None
        self.buffer = b""
        self.nonce = 0

    def open(self):
        for path in socket_paths():
            if not os.path.exists(path):
                continue
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(path)
            except OSError:
                s.close()
                continue
            self.sock, self.buffer, self.nonce = s, b"", 0
            return True
        return False

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None

    def write(self, op, payload):
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        try:
            self.sock.sendall(struct.pack("<II", op, len(data)) + data)
            return True
        except OSError:
            return False

    def _fill(self, timeout):
        ready, _, _ = select.select([self.sock], [], [], max(timeout, 0))
        if not ready:
            return True
        try:
            chunk = self.sock.recv(65536)
        except OSError:
            return False
        if not chunk:
            return False  # Discord closed its end
        self.buffer += chunk
        return True

    def read(self, timeout):
        """(op, dict) of the next frame, (None, None) on a timeout; raises EOFError when gone."""
        deadline = time.monotonic() + timeout
        while True:
            if len(self.buffer) >= 8:
                op, length = struct.unpack_from("<II", self.buffer)
                if length > MAX_FRAME:
                    raise EOFError
                if len(self.buffer) >= 8 + length:
                    body = self.buffer[8:8 + length]
                    self.buffer = self.buffer[8 + length:]
                    try:
                        return op, json.loads(body.decode("utf-8", "replace")) if body else {}
                    except ValueError:
                        return op, {}
            # The socket is polled at least once: with a timeout of 0 (the main loop's _pump),
            # events Discord sent while the launcher was idle (an accepted invite, a join request,
            # a ping) used to stay unread until the next command.
            left = deadline - time.monotonic()
            before = len(self.buffer)
            if not self._fill(max(left, 0)):
                raise EOFError
            if left <= 0 and len(self.buffer) == before:
                return None, None


class Presence:
    def __init__(self):
        self._lock = threading.Condition()
        self._desired = (False, "", None)
        self._desired_key = (False, "", None)
        self._version = 0
        self._stop = False
        self._thread = None
        self._status = OFF
        self._error = ""
        self._joins = []
        self.accept_requests = True

    # --- the window's side -------------------------------------------------------------------

    def update(self, enabled, app_id, activity):
        """Desired presence, called whenever it may have changed: only a change reaches Discord.
        activity None clears it; enabled False (or no app_id) disconnects, which also clears it."""
        key = (bool(enabled), app_id or "", activity.key() if activity else None)
        with self._lock:
            if self._stop:
                return
            if self._thread is None:
                if not enabled:
                    return  # never start the thread for a disabled presence
                self._thread = threading.Thread(target=self._run, name="discord-ipc", daemon=True)
                self._thread.start()
            if key == self._desired_key:
                return
            self._desired_key = key
            self._desired = (bool(enabled), app_id or "", activity)
            self._version += 1
            self._lock.notify_all()

    def take_join(self):
        """The join secret of an invite or "Join" this player accepted in Discord, or None."""
        with self._lock:
            return self._joins.pop(0) if self._joins else None

    def status(self):
        with self._lock:
            return self._status, self._error

    def shutdown(self):
        with self._lock:
            self._stop = True
            self._lock.notify_all()
        if self._thread:
            self._thread.join(timeout=2)

    # --- the thread ----------------------------------------------------------------------------

    def _set_status(self, status, error=""):
        with self._lock:
            self._status, self._error = status, error

    def _request(self, conn, payload, accept):
        """Sends a command and waits for its reply; events that arrive first are handled."""
        conn.nonce += 1
        nonce = str(conn.nonce)
        payload = dict(payload, nonce=nonce)
        if not conn.write(OP_FRAME, payload):
            return None
        deadline = time.monotonic() + REPLY_TIMEOUT
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            op, frame = conn.read(left)
            if op is None:
                return None
            if op == OP_FRAME and frame.get("nonce") == nonce:
                return frame
            if not self._handle(conn, op, frame, accept):
                return None

    def _handle(self, conn, op, frame, accept):
        if op == OP_PING:
            return conn.write(OP_PONG, frame)
        if op == OP_CLOSE:
            return False
        if op != OP_FRAME:
            return True
        evt = frame.get("evt")
        data = frame.get("data") or {}
        if evt == "ACTIVITY_JOIN":
            secret = data.get("secret")
            if secret:
                with self._lock:
                    if len(self._joins) < 8:
                        self._joins.append(secret)
        elif evt == "ACTIVITY_JOIN_REQUEST" and self.accept_requests:
            user = (data.get("user") or {}).get("id")
            if user:
                accept.append(user)
        return True

    def _handshake(self, conn, app_id, accept):
        if not conn.write(OP_HANDSHAKE, {"v": 1, "client_id": app_id}):
            return False
        op, reply = conn.read(REPLY_TIMEOUT)
        if op is None:
            self._set_status(ERROR, "Discord did not answer the handshake")
            return False
        if op != OP_FRAME or reply.get("evt") != "READY":
            self._set_status(ERROR, json.dumps(reply)[:200])  # op 2 CLOSE: e.g. an unknown id
            return False
        for evt in ("ACTIVITY_JOIN", "ACTIVITY_JOIN_REQUEST"):
            if self._request(conn, {"cmd": "SUBSCRIBE", "evt": evt, "args": {}}, accept) is None:
                return False
        return True

    def _set_activity(self, conn, activity, accept):
        payload = {"cmd": "SET_ACTIVITY",
                   "args": {"pid": os.getpid(), "activity": activity.to_json() if activity else None}}
        reply = self._request(conn, payload, accept)
        if reply is None:
            return False
        if reply.get("evt") == "ERROR":
            self._set_status(ERROR, json.dumps(reply.get("data", reply))[:200])  # rejected, still connected
        else:
            self._set_status(CONNECTED)
        return True

    def _pump(self, conn, accept):
        """Whatever Discord sent on its own (events, pings). False when the socket is gone."""
        while True:
            op, frame = conn.read(0)
            if op is None:
                break
            if not self._handle(conn, op, frame, accept):
                return False
        while accept:
            user = accept.pop(0)
            if self._request(conn, {"cmd": "SEND_ACTIVITY_JOIN_INVITE", "args": {"user_id": user}}, accept) is None:
                return False
        return True

    def _run(self):
        conn = _Connection()
        connected_id = ""
        sent = None
        last_attempt = 0.0
        accept = []
        while True:
            with self._lock:
                if self._stop:
                    break
                enabled, app_id, activity = self._desired
                version = self._version
            try:
                if not enabled or not app_id:
                    conn.close()
                    connected_id = ""
                    self._set_status(NO_APP_ID if enabled else OFF)
                else:
                    may_connect = (conn.sock is not None or last_attempt == 0
                                   or time.monotonic() - last_attempt >= RETRY_SECONDS)
                    if may_connect and (sent != version or conn.sock is None):
                        for _attempt in range(2):
                            if conn.sock is None or connected_id != app_id:
                                conn.close()
                                last_attempt = time.monotonic()
                                if not conn.open():
                                    self._set_status(NO_DISCORD)
                                    break
                                if not self._handshake(conn, app_id, accept):
                                    conn.close()
                                    break
                                connected_id = app_id
                            if self._set_activity(conn, activity, accept):
                                sent = version
                                break
                            conn.close()  # Discord restarted: reconnect once and resend
                    if conn.sock is not None and not self._pump(conn, accept):
                        conn.close()
                        sent = None
            except (EOFError, OSError):
                conn.close()
                sent = None
            with self._lock:
                if self._stop:
                    break
                if self._version == version:
                    self._lock.wait(POLL_SECONDS)
        conn.close()  # clears the presence
        self._set_status(OFF)
