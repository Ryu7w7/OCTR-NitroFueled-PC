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

    def test_itemless_ai_motion_hooks(self):
        game = Path(os.environ["BB_TEST_GAME"])
        elf, _, segments, *_ = parse_self((game / "eboot.bin").read_bytes())
        shift = 0x10 if "CUSA14876" in str(game) else 0
        # Kart functions have the same addresses; racer ordering differs by region.
        for address, code in (
            (0xe2f1d0, "55 48 89 e5 41 57 41 56 41 54 53 49 89 ff"),
            (0xe75250 - shift, "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28"),
            (0xe7532d - shift, "e8 1e 04 00 00"), # update entry before expiry
            (0xe2e8e7, "41 8a 47 3c"), # boost-pad flag on acquired boost data
            (0xe2e984, "c4 c1 78 2e 47 0c"), # compare incoming speed before reserve carry
            (0xff11a0 - shift, "55 48 89 e5 41 57 41 56 41 55 41 54 53 50"),
            (0xe8ad70 - shift, "55 48 89 e5 41 57 41 56 53 50 49 89 ff 31 db"),
            (0xbb5060, "c5 fa 10 06 c5 fa 5c 47 60"),
            (0xe33d80, "55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 20"),
            (0xe49d90, "55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 20"),
            (0xe46250, "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 08 01 00 00"),
        ):
            segment = next(p for p in segments if p["type"] in (1, 0x61000010)
                           and p["vaddr"] <= address < p["vaddr"] + p["filesz"])
            offset = segment["offset"] + address - segment["vaddr"]
            expected = bytes.fromhex(code)
            self.assertEqual(elf[offset:offset + len(expected)], expected, hex(address))

    def test_cup_guest_countdown_contract(self):
        game = Path(os.environ["BB_TEST_GAME"])
        elf, _, segments, *_ = parse_self((game / "eboot.bin").read_bytes())
        shift = 0x10 if "CUSA14876" in str(game) else 0
        def read(address, size):
            seg = next(p for p in segments if p["type"] in (1, 0x61000010)
                       and p["vaddr"] <= address < p["vaddr"] + p["filesz"])
            off = seg["offset"] + address - seg["vaddr"]
            return elf[off:off + size]
        import struct
        prefix = bytes.fromhex("55 48 89 e5 41 56 53 8a 05")
        prefix += struct.pack("<i", 0x2a92868 - (0xf3963d - shift))
        prefix += bytes.fromhex("49 89 fe")
        self.assertEqual(read(0xf39630-shift, 16), prefix)
        self.assertEqual(read(0xf398a0-shift, 17), bytes.fromhex(
            "55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18"))
        self.assertEqual(read(0xf344e6-shift, 5), bytes.fromhex("44 39 f2 74 54"))
        self.assertEqual(read(0xf350c0-shift, 18), bytes.fromhex(
            "55 48 89 e5 41 57 41 56 41 54 53 48 8b 5f 28 41 89 d6"))
        self.assertEqual(read(0xf351b0-shift, 14), bytes.fromhex(
            "55 48 89 e5 41 57 41 56 41 54 53 48 89 f3"))
        # Guest's second-stage timer uses the remaining grace duration.
        self.assertEqual(read(0xf398f2-shift, 8), bytes.fromhex("c5 fa 10 80 fc 00 00 00"))
