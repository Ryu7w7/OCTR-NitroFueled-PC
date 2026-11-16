"""CPU-only VFX motion checks; reads owned assets and never submits graphics work."""
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from prepare import parse_self, nid
from archive_reader import Archive
from igz_file import Igz


@unittest.skipUnless(os.environ.get("BB_TEST_GAME"), "requires native USA/EU dump")
class NativeVfxTimestep(unittest.TestCase):
    def test_native_motion_and_scope(self):
        game = Path(os.environ["BB_TEST_GAME"])
        elf, _, segments, *_ = parse_self((game / "eboot.bin").read_bytes())
        dynamic = next(p for p in segments if p["type"] == 2)
        tags = dict(struct.unpack_from("<QQ", elf, at) for at in range(
            dynamic["offset"], dynamic["offset"] + dynamic["filesz"], 16))
        lib = next(p for p in segments if p["type"] == 0x61000000)
        blob = elf[lib["offset"]:lib["offset"] + lib["filesz"]]
        strings = blob[tags[0x61000035]:tags[0x61000035] + tags[0x61000037]]
        symbols = blob[tags[0x61000039]:tags[0x61000039] + tags[0x6100003f]]
        imports = {}
        for start, length in ((0x61000029, 0x6100002d), (0x6100002f, 0x61000031)):
            for at in range(tags[start], tags[start] + tags[length], 24):
                target, info, _ = struct.unpack_from("<QQq", blob, at)
                name = struct.unpack_from("<I", symbols, (info >> 32) * 24)[0]
                imports[target] = strings[name:strings.index(0, name)].decode().split("#")[0]
        # Only these declared scalar math imports are bound to host libm in the probe.
        for address, name in ((0x2808198, "cosf"), (0x28081a0, "sinf"), (0x2808390, "fmodf")):
            self.assertEqual(imports[address], nid(name))
        archive = Archive(game / "archives" / "update2.pak")
        try:
            entry = next(f for f in archive.files if f["path"].lower() ==
                         "vfx/ctr/podium/ctr_h100_gp7_dng_smoke.igz")
            asset = Igz(archive.extract(entry))
        finally:
            archive.data.close()
            archive._file.close()
        self.assertEqual(dict(asset.objects())[0xc0], "igVfxOperatorPrimitiveData")
        self.assertEqual(asset.u64(0xc0 + 0x70), 0x228)
        self.assertEqual(asset.u32(0x228 + 0xc), 15)
        self.assertEqual(asset.u64(0xa50 + 0x20), 0xa90)
        self.assertEqual(dict(asset.objects())[0xa90], "igVfxAnimatedFrame")
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="octr-native-vfx-") as tmp:
            tmp = Path(tmp)
            (tmp / "image.elf").write_bytes(elf)
            (tmp / "smoke.bin").write_bytes(asset.data)
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-ffp-contract=off",
                            "-ffunction-sections", "-fdata-sections", "-I" + str(root / "src"),
                            str(root / "tests/test_vfx_timestep_native.c"), "-Wl,--gc-sections", "-lm",
                            "-o", str(tmp / "probe")], check=True)
            subprocess.run([str(tmp / "probe"), str(tmp / "image.elf"), str(tmp / "smoke.bin")], check=True)
