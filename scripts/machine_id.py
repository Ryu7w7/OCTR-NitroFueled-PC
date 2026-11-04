"""This PC's anonymous fingerprint for the RyuNET server's bans (X-RyuNET-Machine).

A ban on a nickname also covers the PC it played from, so a new nickname or a fresh install
of OCTR does not bring a banned player back. Two parts, each sent only as a SHA-256 digest
(the server can tell the same PC again, never which hardware it is):
- the system: Windows' MachineGuid, Linux's machine-id (a new install of the OS changes it);
- the board: the motherboard's SMBIOS UUID (Windows; Linux only where it is readable), which
  stays through any reinstall. Placeholder UUIDs some boards ship (all zeros, all ones,
  03000200-0400-0500-0006-000700080009) are left out: thousands of PCs share them.
Value: "<system>.<board>", 32 hex digits each, "-" for a part this PC does not give.
Usage: machine_id.py (prints it).
"""
import hashlib
import os
import struct
import sys

SALT = 'octr-machine-v1/'
PLACEHOLDER_UUIDS = {
    bytes(16), b'\xff' * 16,
    bytes.fromhex('00020003000400050006000700080009'),  # 03000200-0400-0500-0006-000700080009
}


def digest(kind, value):
    return hashlib.sha256((SALT + kind + '/' + value).encode()).hexdigest()[:32]


def usable_uuid(raw):
    return len(raw) == 16 and raw not in PLACEHOLDER_UUIDS and len(set(raw)) >= 4


def smbios_uuid(table):
    """The system UUID (SMBIOS structure type 1, offset 8) of a raw SMBIOS table, or None."""
    at = 0
    while at + 4 <= len(table):
        kind, length = table[at], table[at + 1]
        if length < 4:
            return None
        if kind == 1 and length >= 0x18:
            raw = bytes(table[at + 8:at + 24])
            return raw if usable_uuid(raw) else None
        if kind == 127:  # end of table
            return None
        end = table.find(b'\0\0', at + length)  # the structure's strings end with two NULs
        if end < 0:
            return None
        at = end + 2
    return None


def windows_parts():
    system = board = None
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\Microsoft\Cryptography', 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
            guid = str(winreg.QueryValueEx(key, 'MachineGuid')[0]).strip().lower()
            if guid:
                system = guid
    except OSError:
        pass
    try:
        import ctypes
        get_table = ctypes.windll.kernel32.GetSystemFirmwareTable
        get_table.restype = ctypes.c_uint
        provider = 0x52534D42  # 'RSMB'
        size = get_table(provider, 0, None, 0)
        if size:
            buffer = ctypes.create_string_buffer(size)
            if get_table(provider, 0, buffer, size) == size:
                data = buffer.raw
                length = struct.unpack_from('<I', data, 4)[0]  # RawSMBIOSData header: 8 bytes
                raw = smbios_uuid(data[8:8 + length])
                if raw:
                    board = raw.hex()
    except (OSError, AttributeError, struct.error):
        pass
    return system, board


def linux_parts():
    system = board = None
    for path in ('/etc/machine-id', '/var/lib/dbus/machine-id'):
        try:
            value = open(path, encoding='ascii').read().strip().lower()
        except (OSError, UnicodeDecodeError):
            continue
        if len(value) >= 16 and set(value) - {'0'}:
            system = value
            break
    try:  # readable by root only on most systems
        value = open('/sys/class/dmi/id/product_uuid', encoding='ascii').read().strip().lower()
        raw = bytes.fromhex(value.replace('-', ''))
        if usable_uuid(raw):
            board = raw.hex()
    except (OSError, ValueError, UnicodeDecodeError):
        pass
    return system, board


def header():
    """The X-RyuNET-Machine value, or '' when this PC gives nothing to tell it by."""
    system, board = windows_parts() if os.name == 'nt' else linux_parts()
    if not system and not board:
        return ''
    return '.'.join(digest(kind, value) if value else '-' for kind, value in (('system', system), ('board', board)))


if __name__ == '__main__':
    sys.stdout.write(header() + '\n')
