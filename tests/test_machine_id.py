"""The PC fingerprint: SMBIOS parsing, placeholder UUIDs left out, nothing readable sent."""
from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import re
import struct
import unittest
from unittest import mock

import machine_id


def structure(kind, body, strings=()):
    formatted = bytes([kind, 4 + len(body)]) + struct.pack('<H', 0x10) + body
    return formatted + b''.join(s + b'\0' for s in strings) + (b'\0' if strings else b'\0\0')


class MachineIdTests(unittest.TestCase):
    def test_system_uuid_from_smbios(self):
        uuid = bytes(range(0x10, 0x20))
        table = (structure(0, bytes(14), (b'Vendor', b'1.0')) +
                 structure(1, b'\x01\x02\x03\x04' + uuid + bytes(4)) + structure(127, b''))
        self.assertEqual(machine_id.smbios_uuid(table), uuid)

    def test_placeholder_uuids_are_left_out(self):
        for raw in (bytes(16), b'\xff' * 16, bytes.fromhex('00020003000400050006000700080009'),
                    b'\x01\x02' * 8):
            table = structure(1, bytes(4) + raw + bytes(4)) + structure(127, b'')
            self.assertIsNone(machine_id.smbios_uuid(table), raw.hex())

    def test_header_is_two_digests(self):
        with mock.patch.object(machine_id, 'windows_parts', return_value=('guid-123', 'aa' * 16)), \
                mock.patch.object(machine_id, 'linux_parts', return_value=('guid-123', 'aa' * 16)):
            value = machine_id.header()
        self.assertRegex(value, r'^[0-9a-f]{32}\.[0-9a-f]{32}$')
        self.assertNotIn('guid-123', value)
        with mock.patch.object(machine_id, 'windows_parts', return_value=('guid-123', None)), \
                mock.patch.object(machine_id, 'linux_parts', return_value=('guid-123', None)):
            self.assertTrue(re.fullmatch(r'[0-9a-f]{32}\.-', machine_id.header()))
        with mock.patch.object(machine_id, 'windows_parts', return_value=(None, None)), \
                mock.patch.object(machine_id, 'linux_parts', return_value=(None, None)):
            self.assertEqual(machine_id.header(), '')

    def test_this_pc_is_stable(self):
        self.assertEqual(machine_id.header(), machine_id.header())


if __name__ == '__main__':
    unittest.main()
