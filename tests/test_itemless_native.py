"""Itemless contact instruction contracts against a local USA/EU 1.21 dump."""
import os
import unittest
from pathlib import Path
from prepare import parse_self


@unittest.skipUnless(os.environ.get("BB_TEST_GAME"), "requires native USA/EU dump")
class NativeItemless(unittest.TestCase):
    def test_native_hook_contracts(self):
        game = Path(os.environ["BB_TEST_GAME"])
        elf, _, segments, *_ = parse_self((game / "eboot.bin").read_bytes())
        def read(address, size):
            p = next(p for p in segments if p["type"] in (1, 0x61000010) and
                     p["vaddr"] <= address < p["vaddr"] + p["filesz"])
            off = p["offset"] + address - p["vaddr"]
            return elf[off:off + size]
        shift = 0x10 if "CUSA14876" in str(game) else 0
        for address, code in [
            (0xe916b0,"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 08 01 00 00"),
            (0xe92e90,"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28"),
            (0xe916ed,"48 63 44 83 08"), # opposing body index
            (0xe918c8,"43 f6 44 2f 42 04"), # body has entity
            (0xe91904,"4e 8b ac 28 88 00 00 00"), # entity user data
            (0xfd4230,"55 48 89 e5 41 57 41 56 41 55 41 54 53 50"),
        ]:
            expected = bytes.fromhex(code)
            self.assertEqual(read(address - shift, len(expected)), expected, hex(address))
