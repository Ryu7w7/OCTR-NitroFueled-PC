# SPDX-License-Identifier: GPL-2.0-or-later
"""The game's trophies for the launcher's Achievements page: the counterpart of the Windows
launcher's TrophiesTab (launcher/windows/launcher.cpp, v0.4).

The list and the pictures are in sce_sys/trophy/trophy00.trp; the ones unlocked while playing in
<user>/trophies/<TITLE_ID>/unlocked.txt (written by the runtime: src/runtime_services.c). Names,
descriptions and grades are in the TRP's ESFM files, AES-encrypted with the console's trophy key,
which is not part of this port: with the player's own key (the one shadPS4 uses) they are
decrypted as shadPS4 does (src/core/file_format/trp.cpp) and written to trophies.tsv, which the
runtime reads for its in-game notice and the platinum.

No third-party modules: the AppImage's Python has only PyGObject, so AES-128 is done here (a few
KB once per language); the `cryptography` package is used instead when it is installed.
"""
import json
import os
import re
import struct
import time
from pathlib import Path

TRP_MAGIC = 0xDCA24D00
POINTS = {"P": 180, "G": 90, "S": 30, "B": 15}

# --- AES-128 (FIPS-197), CBC without padding ------------------------------------------------

_SBOX = bytes.fromhex(
    "637c777bf26b6fc53001672bfed7ab76ca82c97dfa5947f0add4a2af9ca472c0"
    "b7fd9326363ff7cc34a5e5f171d8311504c723c31896059a071280e2eb27b275"
    "09832c1a1b6e5aa0523bd6b329e32f8453d100ed20fcb15b6acbbe394a4c58cf"
    "d0efaafb434d338545f9027f503c9fa851a3408f929d38f5bcb6da2110fff3d2"
    "cd0c13ec5f974417c4a77e3d645d197360814fdc222a908846eeb814de5e0bdb"
    "e0323a0a4906245cc2d3ac629195e479e7c8376d8dd54ea96c56f4ea657aae08"
    "ba78252e1ca6b4c6e8dd741f4bbd8b8a703eb5664803f60e613557b986c11d9e"
    "e1f8981169d98e949b1e87e9ce5528df8ca1890dbfe6426841992d0fb054bb16")
_INV_SBOX = bytes(_SBOX.index(i) for i in range(256))
_RCON = (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36)


def _xtime(a):
    a <<= 1
    return (a ^ 0x1B) & 0xFF if a & 0x100 else a


def _mul(a, b):
    out = 0
    while b:
        if b & 1:
            out ^= a
        a = _xtime(a)
        b >>= 1
    return out


def _expand(key):
    words = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    for i in range(4, 44):
        w = list(words[i - 1])
        if i % 4 == 0:
            w = w[1:] + w[:1]
            w = [_SBOX[b] for b in w]
            w[0] ^= _RCON[i // 4 - 1]
        words.append([a ^ b for a, b in zip(words[i - 4], w)])
    return [sum(words[r * 4:r * 4 + 4], []) for r in range(11)]


def _encrypt_block(rounds, block):
    s = [a ^ b for a, b in zip(block, rounds[0])]
    for r in range(1, 11):
        s = [_SBOX[b] for b in s]
        s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]  # ShiftRows (column-major state)
        if r < 10:
            mixed = []
            for c in range(4):
                a = s[c * 4:c * 4 + 4]
                mixed += [_mul(a[0], 2) ^ _mul(a[1], 3) ^ a[2] ^ a[3],
                          a[0] ^ _mul(a[1], 2) ^ _mul(a[2], 3) ^ a[3],
                          a[0] ^ a[1] ^ _mul(a[2], 2) ^ _mul(a[3], 3),
                          _mul(a[0], 3) ^ a[1] ^ a[2] ^ _mul(a[3], 2)]
            s = mixed
        s = [a ^ b for a, b in zip(s, rounds[r])]
    return bytes(s)


def _decrypt_block(rounds, block):
    s = [a ^ b for a, b in zip(block, rounds[10])]
    for r in range(9, -1, -1):
        s = [s[(i - 4 * (i % 4)) % 16] for i in range(16)]  # InvShiftRows
        s = [_INV_SBOX[b] for b in s]
        s = [a ^ b for a, b in zip(s, rounds[r])]
        if r > 0:
            mixed = []
            for c in range(4):
                a = s[c * 4:c * 4 + 4]
                mixed += [_mul(a[0], 14) ^ _mul(a[1], 11) ^ _mul(a[2], 13) ^ _mul(a[3], 9),
                          _mul(a[0], 9) ^ _mul(a[1], 14) ^ _mul(a[2], 11) ^ _mul(a[3], 13),
                          _mul(a[0], 13) ^ _mul(a[1], 9) ^ _mul(a[2], 14) ^ _mul(a[3], 11),
                          _mul(a[0], 11) ^ _mul(a[1], 13) ^ _mul(a[2], 9) ^ _mul(a[3], 14)]
            s = mixed
    return bytes(s)


def aes_cbc(key, iv, data, encrypt):
    """AES-128-CBC without padding; data is a multiple of 16 bytes."""
    if len(key) != 16 or len(iv) != 16 or len(data) % 16:
        raise ValueError("AES-128-CBC needs a 16-byte key and IV and whole blocks")
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        cipher = Cipher(algorithms.AES(key), modes.CBC(iv))
        op = cipher.encryptor() if encrypt else cipher.decryptor()
        return op.update(data) + op.finalize()
    except ImportError:
        pass
    rounds = _expand(key)
    out, prev = bytearray(), iv
    for i in range(0, len(data), 16):
        block = data[i:i + 16]
        if encrypt:
            prev = _encrypt_block(rounds, bytes(a ^ b for a, b in zip(block, prev)))
            out += prev
        else:
            out += bytes(a ^ b for a, b in zip(_decrypt_block(rounds, block), prev))
            prev = block
    return bytes(out)


# --- the trophy file and the files shared with the runtime ----------------------------------

def key_bytes(text):
    """The 16-byte trophy key from hex (spaces and separators ignored); None when it is not 32
    hex digits."""
    clean = re.sub(r"[^0-9A-Fa-f]", "", text or "")
    return bytes.fromhex(clean) if len(clean) == 32 else None


def read_trp(game_dir):
    """{entry name: bytes} of the game's trophy00.trp; None when there is none."""
    try:
        data = (Path(game_dir) / "sce_sys" / "trophy" / "trophy00.trp").read_bytes()
    except OSError:
        return None
    if len(data) < 0x60 or struct.unpack_from(">I", data, 0)[0] != TRP_MAGIC:
        return None
    count, entry_size = struct.unpack_from(">II", data, 0x10)
    entries = {}
    for i in range(min(count, 1024)):
        e = 0x60 + i * entry_size
        if e + 48 > len(data):
            break
        name = data[e:e + 32].split(b"\0", 1)[0].decode("ascii", "replace")
        pos, size = struct.unpack_from(">QQ", data, e + 32)
        if pos + size <= len(data):
            entries[name] = data[pos:pos + size]
    return entries


def np_comm_id(game_dir):
    """The NP communication ID (NPWRxxxxx_00) in sce_sys/npbind.dat, needed to decrypt."""
    try:
        data = (Path(game_dir) / "sce_sys" / "npbind.dat").read_bytes()
    except OSError:
        return ""
    found = re.search(rb"NPWR\d{5}_\d{2}", data)
    return found.group().decode() if found else ""


def decrypt_esfm(key, comm_id, entry):
    """One ESFM entry decrypted to its XML (shadPS4's DecryptEFSM): the file key is the NP
    communication ID (16 bytes, zero-padded) encrypted with the trophy key (CBC, zero IV); the
    entry is IV (16 bytes) + data, CBC-decrypted with that key and IV. "" when it does not
    decrypt to trophy XML."""
    if len(entry) <= 32 or (len(entry) - 16) % 16:
        return ""
    comm = comm_id.encode()[:16].ljust(16, b"\0")
    file_key = aes_cbc(key, bytes(16), comm, True)
    xml = aes_cbc(file_key, entry[:16], entry[16:], False)
    end = xml.rfind(b">")
    if end < 0:
        return ""
    text = xml[:end + 1].decode("utf-8", "replace")
    return text if text.startswith("<?xml") or "<trophyconf" in text else ""


def parse_trophy_xml(xml):
    """(title name, {id: (grade, hidden, name, detail)}) of a decrypted TROP*.XML."""
    import xml.etree.ElementTree as ET
    try:
        root = ET.fromstring(xml.encode("utf-8"))
    except ET.ParseError:
        return "", {}
    title = (root.findtext("title-name") or "").strip()
    found = {}
    for trophy in root.iter("trophy"):
        try:
            tid = int(trophy.get("id", ""))
        except ValueError:
            continue
        grade = (trophy.get("ttype") or "?")[:1].upper()
        found[tid] = (grade, trophy.get("hidden") == "yes", (trophy.findtext("name") or "").strip(),
                      (trophy.findtext("detail") or "").strip())
    return title, found


def _clean(text):
    return re.sub(r"[\t\r\n]", " ", text or "")


def read_tsv(path, language):
    """trophies.tsv ("#lang<TAB>code", "#title<TAB>name", then id, grade, hidden, name, detail per
    line): (title, {id: (grade, hidden, name, detail)}) when it is in this language, else None."""
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError):
        return None
    right_language, title, found = False, "", {}
    for line in lines:
        fields = line.split("\t")
        if len(fields) >= 2 and fields[0] == "#lang":
            right_language = fields[1] == language
        elif len(fields) >= 2 and fields[0] == "#title":
            title = fields[1]
        elif len(fields) >= 5 and fields[0].isdigit():
            found[int(fields[0])] = (fields[1][:1] if fields[1] != "?" else "", fields[2] == "1", fields[3], fields[4])
    return (title, found) if right_language and found else None


def write_tsv(path, language, title, items):
    """items: {id: (grade, hidden, name, detail)}. Written whole, then moved in place."""
    out = f"#lang\t{language}\n#title\t{_clean(title)}\n"
    for tid in sorted(items):
        grade, hidden, name, detail = items[tid]
        out += f"{tid}\t{grade or '?'}\t{'1' if hidden else '0'}\t{_clean(name)}\t{_clean(detail)}\n"
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(out, encoding="utf-8")
    os.replace(tmp, path)


def read_unlocked(path):
    """{id: unix time} of unlocked.txt ("<id> <time>" lines; 1 when the time is unknown)."""
    found = {}
    try:
        lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return found
    for line in lines:
        parts = line.split()
        if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
            found.setdefault(int(parts[0]), int(parts[1]) or 1)
    return found


def reset_unlocked(path):
    """unlocked.txt moved aside (<name>.<time>.bak), as the Windows launcher's Reset."""
    path = Path(path)
    if path.is_file():
        path.rename(path.with_name(f"{path.name}.{int(time.time())}.bak"))


# --- shadPS4's key ----------------------------------------------------------------------------

def shadps4_dirs():
    """shadPS4's user folders on Linux: $XDG_DATA_HOME/shadPS4 (else ~/.local/share/shadPS4,
    common/path_util.cpp) and the Flatpak's."""
    home = Path.home()
    data = os.environ.get("XDG_DATA_HOME") or str(home / ".local" / "share")
    dirs = [Path(data) / "shadPS4", home / ".local" / "share" / "shadPS4",
            home / ".var" / "app" / "net.shadps4.shadPS4" / "data" / "shadPS4"]
    return list(dict.fromkeys(dirs))


def key_from_file(path):
    """The trophy key in a shadPS4 keys.json (TrophyKeySet.ReleaseTrophyKey: hex or a byte array)
    or an older config.toml ([Keys] TrophyKey = "..."); "" when it has none."""
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    try:
        data = json.loads(text)
    except ValueError:
        data = None
    if isinstance(data, dict):
        value = (data.get("TrophyKeySet") or {}).get("ReleaseTrophyKey") if isinstance(data.get("TrophyKeySet"), dict) \
            else data.get("ReleaseTrophyKey")
        if isinstance(value, list):
            value = "".join(f"{int(b) & 0xFF:02x}" for b in value if isinstance(b, int))
        return value if isinstance(value, str) and key_bytes(value) else ""
    found = re.search(r'(?:Release)?TrophyKey"?\s*[:=]\s*"([0-9A-Fa-f]{32})"', text)
    return found.group(1) if found else ""


def shadps4_key():
    """(key in hex, file it came from) of the first shadPS4 install with one; ("", None) if none."""
    for folder in shadps4_dirs():
        for name in ("keys.json", "config.toml"):
            key = key_from_file(folder / name)
            if key:
                return key, folder / name
    return "", None


# --- everything the page shows ----------------------------------------------------------------

class TrophySet:
    """problem: "" | "no_game" | "no_trp"; key_problem: "" | "bad_key" | "no_comm_id"."""

    def __init__(self):
        self.title_id = ""
        self.language = ""
        self.title_name = ""
        self.icon = None        # ICON0.PNG bytes
        self.trophies = []      # dicts: id, grade, hidden, name, detail, icon (PNG bytes), unlocked
        self.names = False      # names and grades known
        self.problem = ""
        self.key_problem = ""
        self.folder = None      # <user>/trophies/<TITLE_ID>

    def refresh_unlocked(self):
        unlocked = read_unlocked(self.folder / "unlocked.txt") if self.folder else {}
        for t in self.trophies:
            t["unlocked"] = unlocked.get(t["id"], 0)


def load(game_dir, user_dir, title_id, language, key_hex):
    """The trophies of the game in game_dir, with their names when trophies.tsv has them in this
    language or the key decrypts them (then trophies.tsv is written for the runtime)."""
    t = TrophySet()
    t.title_id, t.language = title_id, language
    t.folder = Path(user_dir) / "trophies" / title_id
    if not game_dir or not (Path(game_dir) / "eboot.bin").is_file():
        t.problem = "no_game"
        return t
    entries = read_trp(game_dir)
    if entries is None:
        t.problem = "no_trp"
        return t
    for name, data in entries.items():
        match = re.fullmatch(r"TROP(\d{3})\.PNG", name)
        if match:
            t.trophies.append({"id": int(match.group(1)), "grade": "", "hidden": False, "name": "",
                               "detail": "", "icon": data, "unlocked": 0})
    t.trophies.sort(key=lambda x: x["id"])
    t.icon = entries.get("ICON0.PNG")
    known = read_tsv(t.folder / "trophies.tsv", language)
    key = key_bytes(key_hex)
    if known is None and key:
        comm = np_comm_id(game_dir)
        try:
            localized = f"TROP_{int(language):02d}.ESFM"
        except ValueError:
            localized = "TROP.ESFM"
        name = localized if localized in entries else "TROP.ESFM"
        xml = decrypt_esfm(key, comm, entries[name]) if comm and name in entries else ""
        if xml:
            title, items = parse_trophy_xml(xml)
            if items:
                known = (title, items)
                try:
                    write_tsv(t.folder / "trophies.tsv", language, title, items)
                except OSError:
                    pass  # shown anyway; the runtime keeps its own names
        if known is None:
            t.key_problem = "no_comm_id" if not comm else "bad_key"
    if known is not None:
        t.title_name, items = known
        for tr in t.trophies:
            if tr["id"] in items:
                tr["grade"], tr["hidden"], tr["name"], tr["detail"] = items[tr["id"]]
        t.names = True
    t.refresh_unlocked()
    return t
