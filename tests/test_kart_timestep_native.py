"""Offline kart speed, rotation, camera and solver checks against a USA/EU dump."""
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from prepare import parse_self, nid


@unittest.skipUnless(os.environ.get("BB_TEST_GAME"), "requires native USA/EU dump")
class NativeKartTimestep(unittest.TestCase):
    def test_native_speed_and_solver(self):
        from archive_reader import Archive
        from igz_file import Igz

        game = Path(os.environ["BB_TEST_GAME"])
        elf, _, segments, *_ = parse_self((game / "eboot.bin").read_bytes())
        archive = Archive(game / "archives" / "update2.pak")
        try:
            entry = next(f for f in archive.files if f["path"].lower() == "loosedata/drivingarchetypes.igz")
            data = Igz(archive.extract(entry))
        finally:
            archive.data.close()
            archive._file.close()
        objects = [offset for offset, kind in data.objects() if kind == "CKartLinearVelocityData"]
        fixtures = bytearray(struct.pack("<I", len(objects)))
        for offset in objects:
            fixtures += data.data[offset:offset + 48]
            for field in (0x18, 0x20, 0x28):
                curve = data.u64(offset + field)
                fixtures += data.data[curve:curve + 104]
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="octr-native-speed-") as tmp:
            tmp = Path(tmp)
            (tmp / "image.elf").write_bytes(elf)
            (tmp / "curves.bin").write_bytes(fixtures)
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
                            "-ffunction-sections", "-fdata-sections", "-I" + str(root / "src"),
                            str(root / "tests/test_kart_speed_native.c"), "-Wl,--gc-sections", "-lm",
                            "-o", str(tmp / "probe")], check=True)
            subprocess.run([str(tmp / "probe"), str(tmp / "image.elf"), str(tmp / "curves.bin")], check=True)
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
                            "-ffunction-sections", "-fdata-sections", "-I" + str(root / "src"),
                            str(root / "tests/test_camera_timestep_native.c"), "-Wl,--gc-sections", "-lm",
                            "-o", str(tmp / "camera")], check=True)
            subprocess.run([str(tmp / "camera"), str(tmp / "image.elf")], check=True)

            # Resolve the original import relocations before supplying host math.
            dynamic = next(p for p in segments if p["type"] == 2)
            tags = dict(struct.unpack_from("<QQ", elf, at) for at in
                        range(dynamic["offset"], dynamic["offset"] + dynamic["filesz"], 16))
            library = next(p for p in segments if p["type"] == 0x61000000)
            blob = elf[library["offset"]:library["offset"] + library["filesz"]]
            strings = blob[tags[0x61000035]:tags[0x61000035] + tags[0x61000037]]
            imports = {}
            for offset_tag, size_tag in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
                for at in range(tags[offset_tag], tags[offset_tag] + tags[size_tag], 24):
                    target, info, _ = struct.unpack_from("<QQq", blob, at)
                    if info & 0xffffffff in (1, 6, 7):
                        name, = struct.unpack_from("<I", blob, tags[0x61000039] + (info >> 32) * 24)
                        imports[target] = strings[name:strings.index(0, name)].decode().split("#")[0]

            def read(address, size):
                segment = next(p for p in segments if p["type"] in (1, 0x61000010) and
                               p["vaddr"] <= address < p["vaddr"] + p["filesz"])
                offset = segment["offset"] + address - segment["vaddr"]
                return elf[offset:offset + size]

            for address, name in ((0xe462fe, "sinf"), (0xe46313, "cosf")):
                call = read(address, 5)
                self.assertEqual(call[0], 0xe8)
                thunk = address + 5 + struct.unpack_from("<i", call, 1)[0]
                instruction = read(thunk, 6)
                self.assertEqual(instruction[:2], b"\xff\x25")
                slot = thunk + 6 + struct.unpack_from("<i", instruction, 2)[0]
                self.assertEqual(imports[slot], nid(name))

            motion = bytearray(struct.pack("<I", len(data.data))) + data.data
            for offsets in (data.sites["ROFS"], [at for at, kind in data.objects()
                                               if kind == "CDrivingArchetypeData"]):
                motion += struct.pack("<I", len(offsets))
                motion += struct.pack("<" + "I" * len(offsets), *offsets)
            (tmp / "motion.bin").write_bytes(motion)
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
                            "-ffunction-sections", "-fdata-sections", "-I" + str(root / "src"),
                            str(root / "tests/test_kart_motion_native.c"), "-Wl,--gc-sections", "-lm",
                            "-o", str(tmp / "motion")], check=True)
            subprocess.run([str(tmp / "motion"), str(tmp / "image.elf"), str(tmp / "motion.bin")], check=True)

    def test_contact_timestep_contract(self):
        import struct
        game = Path(os.environ["BB_TEST_GAME"])
        elf, _, segments, *_ = parse_self((game / "eboot.bin").read_bytes())
        def read(address, size):
            segment = next(p for p in segments if p["type"] in (1, 0x61000010) and
                           p["vaddr"] <= address < p["vaddr"] + p["filesz"])
            offset = segment["offset"] + address - segment["vaddr"]
            return elf[offset:offset + size]
        # USA/EU share the code address, but not the RIP-relative constant address.
        code = read(0xe47d1c, 18)
        self.assertEqual(code[:4], bytes.fromhex("c5 fa 59 05"))
        self.assertEqual(code[8:], bytes.fromhex("c5 fa 11 45 84 c5 f8 2e 45 b8"))
        constant = 0xe47d24 + struct.unpack_from("<i", code, 4)[0]
        self.assertEqual(read(constant, 4), struct.pack("<f", 30))
        self.assertEqual(read(0xe47d2e, 4), bytes.fromhex("48 8d 45 84"))
        self.assertEqual(read(0xe46278, 7), bytes.fromhex("48 89 95 58 ff ff ff"))
        self.assertEqual(read(0xe252c9, 4), bytes.fromhex("45 8b 6f 18"))
        self.assertEqual(read(0xe46b2a, 15), bytes.fromhex(
            "c5 fa 10 7d c0 c5 fa 10 5d c4 c5 fa 10 45 c8"))
        self.assertEqual(read(0xe47237, 15), bytes.fromhex(
            "c5 fa 10 6d c0 c5 fa 10 75 c4 c5 7a 10 4d c8"))
        self.assertEqual(read(0xe2f821, 15), bytes.fromhex(
            "c5 fa 59 c2 c5 f2 58 c0 c4 c1 7a 11 44 24 58"))
        self.assertEqual(read(0xe2f6a8, 5), bytes.fromhex("c5 fa 11 45 ac"))
