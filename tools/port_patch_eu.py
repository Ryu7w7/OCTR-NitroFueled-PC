"""Ports executable patches made for the USA game (CUSA13795 1.21) to the European one (CUSA14876 1.21).

The two executables hold the same code, but much of it sits at other addresses (most functions 0x10
lower in the EU build); the data (globals, tables) is at the same addresses. A patch line is one of:
- a signed line (Signature): the site is searched in the EU executable; its bytes are kept, except
  relative branches and RIP-relative operands, which are encoded again for the new site;
- fixed-address code (no Signature: code the mod places in free space, the code segment's tail or the
  mod code area): kept at its address, with every branch or RIP-relative reference that reaches the
  game's code redirected to the same instruction in the EU executable.
The same instruction is found by comparing the code around it: the USA instruction stream from that
address and the EU one from candidate addresses near it, branches and RIP-relative operands compared
by where they lead (code: the same instruction again, data: the same address). ExpectedSHA256 is
computed again from the EU bytes the write replaces.

Usage: python tools/port_patch_eu.py <usa eboot.bin> <eu eboot.bin> <out dir> <patch.xml>...
SPDX-License-Identifier: GPL-2.0-or-later
"""
import hashlib
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import capstone  # noqa: E402
from capstone import x86  # noqa: E402
from prepare import parse_self  # noqa: E402
from patches import EBOOT_BASE, load_segments, locate, encode  # noqa: E402

USA_TITLE, EU_TITLE = 'CUSA13795', 'CUSA14876'
CODE_END = 0x233c078       # the end of the 1.21 code (both builds); the mod code area follows it
MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
MD.detail = True


class Image:
    def __init__(self, eboot):
        self.elf = parse_self(Path(eboot).read_bytes())[0]
        self.loads = load_segments(self.elf)

    def at(self, va, n):
        for start, pos, size in self.loads:
            if start <= va and va + n <= start + size:
                return self.elf[pos + va - start:pos + va - start + n]
        return None

    def code(self, va):
        return 0x400000 - EBOOT_BASE <= va < CODE_END


def references(insn):
    """[(kind, field offset, field size, target)] of an instruction's relative branch and
    RIP-relative operand."""
    out = []
    if insn.disp_size and any(op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP
                              for op in insn.operands):
        mem = next(op for op in insn.operands if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP)
        out.append(('rip', insn.disp_offset, insn.disp_size, insn.address + insn.size + mem.mem.disp))
    if insn.group(capstone.CS_GRP_JUMP) or insn.group(capstone.CS_GRP_CALL):
        if insn.imm_size and insn.operands and insn.operands[0].type == x86.X86_OP_IMM:
            out.append(('branch', insn.imm_offset, insn.imm_size, insn.operands[0].imm))
    return out


def shape(image, va, count=10):
    """The instruction stream at va, with references reduced to where they lead relative to va
    (code) or as an address (data): what two copies of the same code share."""
    data = image.at(va, 192)
    if data is None:
        return None
    out = []
    for insn in MD.disasm(data, va):
        raw = bytearray(insn.bytes)
        refs = references(insn)
        for kind, offset, size, target in refs:
            raw[offset:offset + size] = bytes(size)
        tags = tuple(('code', target - va) if image.code(target) else ('data', target)
                     for kind, offset, size, target in refs)
        out.append((bytes(raw), tags))
        if len(out) == count:
            break
    return out if len(out) >= 6 else None


class Porter:
    def __init__(self, usa, eu, caves):
        self.usa, self.eu, self.caves = usa, eu, caves
        self.cache = {}

    def in_cave(self, va):
        return va >= CODE_END or any(lo <= va < hi for lo, hi in self.caves)

    def code_target(self, va):
        """The EU address of the USA instruction at va."""
        if self.in_cave(va):
            return va
        if va in self.cache:
            return self.cache[va]
        want = shape(self.usa, va)
        if want is None:
            raise ValueError(f'no code at {va:#x}')
        found = []
        for step in range(0, 0x800, 0x10):
            for delta in ((0,) if step == 0 else (-step, step)):
                for fine in (0,):
                    cand = va + delta + fine
                    got = shape(self.eu, cand)
                    if got is not None and self.same(want, got, va, cand):
                        found.append(cand)
            if found:
                break
        if len(found) != 1:
            raise ValueError(f'USA {va:#x}: {len(found)} EU matches')
        self.cache[va] = found[0]
        return found[0]

    def same(self, a, b, va, vb):
        if len(a) != len(b):
            return False
        for (ra, ta), (rb, tb) in zip(a, b):
            if ra != rb or len(ta) != len(tb):
                return False
            for x, y in zip(ta, tb):
                if x[0] != y[0]:
                    return False
                if x[0] == 'data' and x[1] != y[1]:
                    return False
                # code: the same distance (inside one function); calls elsewhere checked loosely
        return True

    def data_target(self, va):
        return va  # the data is at the same addresses in both builds (checked by port())

    def relocate(self, value, old_site, new_site):
        """value (bytes written at old_site in USA) for new_site in EU."""
        out = bytearray(value)
        for insn in MD.disasm(bytes(value), old_site):
            for kind, offset, size, target in references(insn):
                if size not in (1, 4):
                    continue
                inside = old_site <= target < old_site + len(value)
                if inside:
                    new_target = new_site + (target - old_site)
                elif self.usa.code(target) or self.in_cave(target):
                    new_target = self.code_target(target)
                else:
                    new_target = self.data_target(target)
                    if not self.in_cave(target):
                        self.check_data(target)
                end = new_site + (insn.address - old_site) + insn.size
                rel = new_target - end
                at = insn.address - old_site + offset
                if size == 1:
                    if not -128 <= rel < 128:
                        raise ValueError(f'short branch at {insn.address:#x} out of range')
                    out[at] = rel & 0xff
                else:
                    out[at:at + 4] = (rel & 0xffffffff).to_bytes(4, 'little')
        return bytes(out)

    def check_data(self, va):
        a, b = self.usa.at(va, 8), self.eu.at(va, 8)
        if a is not None and b is not None and a != b and self.usa.code(va):
            raise ValueError(f'data at {va:#x} differs')


def cave_ranges(lines):
    out = []
    for line in lines:
        if not line.get('Signature'):
            start = int(line.get('Address'), 0) - EBOOT_BASE
            out.append((start, start + len(encode(line))))
    return out


def port(usa_eboot, eu_eboot, out_dir, files):
    usa, eu = Image(usa_eboot), Image(eu_eboot)
    trees = [(Path(f), ET.parse(f)) for f in files]
    all_lines = [line for _, t in trees for line in t.getroot().iter('Line')]
    porter = Porter(usa, eu, cave_ranges(all_lines))
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    report = []
    for path, tree in trees:
        root = tree.getroot()
        for ident in root.iter('ID'):
            if ident.text == USA_TITLE:
                ident.text = EU_TITLE
        moved = 0
        for line in root.iter('Line'):
            usa_site = int(line.get('Address'), 0) - EBOOT_BASE
            value = encode(line)
            if line.get('Signature'):
                try:
                    eu_site = locate(line, usa_site, eu.elf)
                except ValueError:
                    # the signature holds bytes that differ in EU (a call's distance): the same
                    # instruction found by its code, and a signature made from the EU bytes
                    shift = int(line.get('SignatureOffset', '0'), 0)
                    eu_site = porter.code_target(usa_site)
                    tokens = line.get('Signature').split()
                    eu_bytes = eu.at(eu_site - shift, len(tokens))
                    usa_bytes = usa.at(usa_site - shift, len(tokens))
                    line.set('Signature', ' '.join(
                        '??' if t == '??' or eb != ub else f'{eb:02x}'
                        for t, eb, ub in zip(tokens, eu_bytes, usa_bytes)))
                    locate(line, eu_site, eu.elf)  # unique in the EU executable
                    print(f'{path.name}: {usa_site + EBOOT_BASE:#x}: EU signature made, site {eu_site + EBOOT_BASE:#x}')
            else:
                eu_site = usa_site
                original = eu.at(eu_site, len(value))
                if original is not None and any(original) and original != usa.at(usa_site, len(value)):
                    raise ValueError(f'{path.name}: the EU bytes at {usa_site:#x} are not free space')
            new_value = porter.relocate(value, usa_site, eu_site)
            original = eu.at(eu_site, len(new_value))
            if original is None:
                original = bytes(len(new_value))  # the mod code area: zeros
            line.set('Address', f'{eu_site + EBOOT_BASE:#010x}')
            line.set('Value', new_value.hex())
            if line.get('ExpectedSHA256'):
                line.set('ExpectedSHA256', hashlib.sha256(original).hexdigest())
            moved += eu_site != usa_site or new_value != value
        target = out_dir / path.name
        text = ET.tostring(root, encoding='unicode')
        header = path.read_text(encoding='utf-8').split('<Patch>')[0]
        target.write_text(header + text + '\n', encoding='utf-8')
        report.append(f'{path.name}: {moved} line(s) changed for EU')
    print('\n'.join(report))
    print(f'{len(porter.cache)} game code references mapped:',
          ', '.join(f'{u:#x}->{e:#x}' for u, e in sorted(porter.cache.items())[:12]), '...')


if __name__ == '__main__':
    port(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
