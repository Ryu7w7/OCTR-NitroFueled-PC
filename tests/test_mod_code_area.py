"""The executable segment is extended up to the next segment for patch mods' code."""
from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import hashlib
import struct
import unittest
import xml.etree.ElementTree as ET

import patches

# USA/EU 1.21 layout: code 0..0x233c078, SCE relro from 0x2400000, data from 0x2a00000.
LOADS = [(1, 5, 0, 0x233c078), (0x61000010, 4, 0x2400000, 0x600000), (1, 6, 0x2a00000, 0x1ee66d)]


def elf_with(loads, code_bytes):
    """A minimal ELF: program headers for loads, file bytes only for the code segment."""
    phoff, size = 0x40, 56
    head = bytearray(0x1000)
    struct.pack_into('<Q', head, 0x20, phoff)
    struct.pack_into('<HH', head, 0x36, size, len(loads))
    for i, (kind, flags, vaddr, memsz) in enumerate(loads):
        filesz = len(code_bytes) if vaddr == 0 else 0
        struct.pack_into('<IIQQQQQQ', head, phoff + i * size, kind, flags, 0x1000 if vaddr == 0 else 0,
                         vaddr, vaddr, filesz, memsz, 0x4000)
    return bytes(head) + code_bytes


class ModCodeAreaTests(unittest.TestCase):
    def test_extends_code_to_the_next_segment(self):
        self.assertEqual(patches.code_extension(LOADS), 0x2400000)
        self.assertIsNone(patches.code_extension([(1, 5, 0, 0x100000), (1, 6, 0x400000, 0x1000)]))  # gap too big

    def test_write_in_the_area_is_checked_against_zeros(self):
        elf = elf_with([(1, 5, 0, 0x2000), (0x61000010, 4, 0x4000, 0x1000)], b'\x90' * 0x2000)
        segments = patches.eboot_segments(elf)
        self.assertIn((0, 0x4000), segments)
        data = bytes.fromhex('c3c3')
        line = ET.Element('Line', Type='bytes', Address=hex(0x3000 + patches.EBOOT_BASE), Value='c3c3',
                          ExpectedSHA256=hashlib.sha256(bytes(2)).hexdigest())
        meta = ET.Element('Metadata'); meta.append(line)
        writes = patches.compile_external([('mod.xml/Mod', None, meta)], segments, elf, strict=True)
        self.assertEqual(writes, [(0x3000, data)])
        line.set('Address', hex(0x5000 + patches.EBOOT_BASE))  # inside the next segment's range? no: past it
        with self.assertRaises(ValueError):
            patches.compile_external([('mod.xml/Mod', None, meta)], segments, elf, strict=True)


if __name__ == '__main__':
    unittest.main()
