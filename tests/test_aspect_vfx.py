"""Execute the 16:10 GUI VFX patch's actual bytes without a game or ISA package."""
from paths import ROOT
import math
import struct
import unittest
import xml.etree.ElementTree as ET


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def vfx_line():
    lines = list(ET.parse(ROOT / 'scripts/aspect_ratio_data/16x10.xml').iter('Line'))
    found = [line for line in lines if int(line.get('Address'), 0) == 0x659458]
    if len(found) != 1:
        raise AssertionError('Expected one signed GUI VFX normalization block')
    return found[0]


class VfxMachine:
    """Focused interpreter for the VEX float instructions used by this patch.

    Decode VEX/ModRM register operands, signed displacements and immediate bits;
    do not substitute the desired coordinate formula for the instruction stream.
    Code memory is read-only and point accesses are checked against three vec4s.
    Everything outside that small supported instruction set fails closed.
    """
    def __init__(self, code, points, canvas, depth, code_base=0x259458):
        self.code = bytes(code)
        self.base = code_base
        self.pc = 0
        self.point_base = 0x10000000
        self.guard = bytes.fromhex('123456789abcdef0') * 2
        self.memory = bytearray(self.guard + b''.join(struct.pack('<4f', *p) for p in points) + self.guard)
        if len(points) != 3:
            raise AssertionError('The native call consumes three corner vectors')
        self.gpr = [0x60000000 + i * 0x100 for i in range(16)]
        self.gpr[1] = 3                       # ecx: retained native call's count
        self.gpr[2] = self.point_base         # rdx: output, aliases input
        self.gpr[6] = self.point_base         # rsi: input
        self.gpr[7] = 0x70000000              # rdi: retained camera-matrix argument
        self.xmm = [[f32(100 * r + lane + .25) for lane in range(4)] for r in range(16)]
        self.xmm[2][0], self.xmm[3][0] = -1., 1.
        self.xmm[4] = [0., 0., 0., 1.]
        self.xmm[5][0] = f32(depth)
        self.xmm[6][0], self.xmm[7][0] = f32(2 / canvas[0]), f32(-2 / canvas[1])
        self.original_gpr = self.gpr.copy()
        self.original_xmm = [r.copy() for r in self.xmm]
        self.reads, self.writes, self.jumps = [], [], []

    def fetch(self, count):
        if not 0 <= self.pc <= len(self.code) - count:
            raise AssertionError('Instruction reads outside replacement')
        value = self.code[self.pc:self.pc + count]
        self.pc += count
        return value

    def read(self, address, count):
        self.reads.append((address, count))
        if self.base <= address <= self.base + len(self.code) - count:
            return self.code[address - self.base:address - self.base + count]
        if self.point_base <= address <= self.point_base + 48 - count:
            offset = len(self.guard) + address - self.point_base
            return self.memory[offset:offset + count]
        raise AssertionError('Read outside the inline constant and three point vectors')

    def write(self, address, value):
        if not self.point_base <= address <= self.point_base + 48 - len(value):
            raise AssertionError('Write outside the three point vectors')
        self.writes.append((address, len(value)))
        offset = len(self.guard) + address - self.point_base
        self.memory[offset:offset + len(value)] = value

    def operand(self, modrm, b_extension):
        mode, register = modrm >> 6, (modrm & 7) + b_extension
        if mode == 3:
            return 'register', register
        if modrm & 7 == 4:
            raise AssertionError('Unexpected SIB operand')
        if mode == 0 and modrm & 7 == 5:
            displacement = struct.unpack('<i', self.fetch(4))[0]
            return 'rip', self.pc + displacement
        displacement = (struct.unpack('<b', self.fetch(1))[0] if mode == 1 else
                        struct.unpack('<i', self.fetch(4))[0] if mode == 2 else 0)
        return 'memory', self.gpr[register] + displacement

    def values(self, operand, count):
        kind, location = operand
        if kind == 'register':
            return self.xmm[location][:count]
        address = self.base + location if kind == 'rip' else location
        return list(struct.unpack('<' + str(count) + 'f', self.read(address, count * 4)))

    def run(self):
        steps = 0
        while self.pc < len(self.code):
            steps += 1
            if steps > 64:
                raise AssertionError('Replacement does not terminate')
            start = self.pc
            lead = self.fetch(1)[0]
            if lead in (0xeb, 0xe9):
                displacement = struct.unpack('<b' if lead == 0xeb else '<i',
                                             self.fetch(1 if lead == 0xeb else 4))[0]
                self.pc += displacement
                self.jumps.append((start, self.pc))
                if self.pc != len(self.code):
                    raise AssertionError('Jump must reach the retained native call exactly')
                continue
            if lead == 0xc5:
                vex = self.fetch(1)[0]
                opcode_map, r_extension, b_extension = 1, ((~vex >> 7) & 1) * 8, 0
            elif lead == 0xc4:
                extensions, vex = self.fetch(2)
                opcode_map = extensions & 31
                r_extension, b_extension = ((~extensions >> 7) & 1) * 8, ((~extensions >> 5) & 1) * 8
                if vex & 0x80:
                    raise AssertionError('Unexpected VEX.W operand')
            else:
                raise AssertionError(f'Unexpected opcode at {start:#x}')
            if vex & 4:
                raise AssertionError('Expected 128-bit vectors')
            prefix, source = vex & 3, (~vex >> 3) & 15
            opcode, modrm = self.fetch(2)
            target = ((modrm >> 3) & 7) + r_extension
            operand = self.operand(modrm, b_extension)
            key = opcode_map, prefix, opcode
            a = self.xmm[source].copy()
            if key == (1, 0, 0x14):             # vunpcklps
                b = self.values(operand, 4)
                self.xmm[target] = [a[0], b[0], a[1], b[1]]
            elif key == (1, 0, 0x16) and operand[0] == 'register':  # vmovlhps
                b = self.values(operand, 4)
                self.xmm[target] = a[:2] + b[:2]
            elif key == (1, 2, 0x59):           # vmulss: upper lanes from src1
                self.xmm[target] = [f32(a[0] * self.values(operand, 1)[0])] + a[1:]
            elif key == (3, 1, 0x21):           # vinsertps: select source/dest, apply zero mask
                immediate = self.fetch(1)[0]
                if operand[0] == 'rip':
                    operand = 'rip', operand[1] + 1  # RIP follows the immediate byte too
                b = self.values(operand, 4 if operand[0] == 'register' else 1)
                a[(immediate >> 4) & 3] = b[immediate >> 6] if operand[0] == 'register' else b[0]
                self.xmm[target] = [0. if immediate & (1 << lane) else value
                                    for lane, value in enumerate(a)]
            elif key == (1, 2, 0x7e):           # vmovq: read XY only and zero ZW
                self.xmm[target] = self.values(operand, 2) + [0., 0.]
            elif key in ((1, 0, 0x59), (1, 0, 0x58)):
                b = self.values(operand, 4)
                self.xmm[target] = [f32(x * y if opcode == 0x59 else x + y) for x, y in zip(a, b)]
            elif key == (1, 0, 0x11) and operand[0] == 'memory':  # vmovups store
                self.write(operand[1], struct.pack('<4f', *self.xmm[target]))
            else:
                raise AssertionError(f'Unexpected VEX instruction at {start:#x}: {key}')
        if len(self.jumps) != 1:
            raise AssertionError('Inline factor data was not skipped once')
        return [struct.unpack_from('<4f', self.memory, len(self.guard) + offset)
                for offset in (0, 16, 32)]


class AspectVfxTests(unittest.TestCase):
    def setUp(self):
        line = vfx_line()
        self.code = bytes.fromhex(line.get('Value'))
        self.assertEqual(len(self.code), len(bytes.fromhex(line.get('Signature'))))
        self.assertEqual(len(self.code), 150)

    def check_points(self, points, canvas, depth, code_base=0x259458):
        machine = VfxMachine(self.code, points, canvas, depth, code_base)
        result = machine.run()
        for old, new in zip(points, result):
            # Independent logical-canvas contract; the interpreter knows only opcodes.
            expected_x = .9 * (2 * f32(old[0]) / canvas[0] - 1)
            expected_y = f32(f32(f32(old[1]) * f32(-2 / canvas[1])) + 1)
            self.assertAlmostEqual(new[0], expected_x, delta=2e-6)
            self.assertEqual(new[1:], (expected_y, f32(depth), 1.))
            self.assertTrue(all(math.isfinite(value) for value in new))
        self.assertEqual(machine.gpr, machine.original_gpr)
        for register in (1, 2, 3, 5, 7, *range(8, 16)):
            self.assertEqual(machine.xmm[register], machine.original_xmm[register])
        self.assertEqual(machine.memory[:len(machine.guard)], machine.guard)
        self.assertEqual(machine.memory[-len(machine.guard):], machine.guard)
        self.assertEqual(machine.writes, [(machine.point_base + offset, 16) for offset in (0, 16, 32)])
        point_reads = [(at - machine.point_base, count) for at, count in machine.reads
                       if machine.point_base <= at < machine.point_base + 48]
        self.assertEqual(point_reads, [(0, 8), (16, 8), (32, 8)])
        code_reads = [(at - code_base, count) for at, count in machine.reads
                      if code_base <= at < code_base + len(self.code)]
        self.assertEqual(code_reads, [(146, 4), (146, 4)])
        return result

    def test_screen_edges_center_and_all_three_corners(self):
        for canvas in ((1920, 1080), (960, 1080), (1920, 540), (960, 540),
                       (1280, 720), (2560, 1440)):
            width, height = canvas
            points = [(0, height, 37, -4), (width / 2, height / 2, -71, 8), (width, 0, 91, -3)]
            with self.subTest(canvas=canvas):
                result = self.check_points(points, canvas, .375)
                for point, expected_x, expected_y in zip(result, (-.9, 0, .9), (-1, 0, 1)):
                    self.assertAlmostEqual(point[0], expected_x, places=6)
                    self.assertAlmostEqual(point[1], expected_y, places=6)

    def test_discards_nonfinite_input_zw_and_preserves_calculated_depth(self):
        for depth in (-3.5, 0., .083, 1.25):
            for ignored in ((math.nan, math.inf), (-math.inf, math.nan), (math.nan, math.nan)):
                with self.subTest(depth=depth, ignored=ignored):
                    self.check_points([(100, 250, *ignored), (900, -120, *ignored),
                                       (2050, 1300, *ignored)], (1920, 1080), depth)

    def test_forward_projection_restores_host_positions_and_dynamic_fill(self):
        # Scan a 410-unit bar at different host positions/scales. Include split
        # and off-center hosts so the expected correction cannot be a fixed shift.
        for canvas in ((1920, 1080), (1920, 540), (960, 1080), (2560, 1440)):
            for host_x, host_y, scale in ((1674, 982, .75), (1862, 503, .75),
                                         (924.5, 497.8, .6), (879.5, 1032.3, .6),
                                         (canvas[0] / 2, canvas[1] / 2, 1.)):
                for fill in (0., .05, .25, .5, .75, .95, 1.):
                    x, y = host_x + scale * (410 * fill - 205), host_y
                    corners = [(x - 5 * scale, y - 10 * scale, math.nan, math.inf),
                               (x + 5 * scale, y - 10 * scale, math.nan, math.inf),
                               (x + 5 * scale, y + 10 * scale, math.nan, math.inf)]
                    with self.subTest(canvas=canvas, host=(host_x, host_y, scale), fill=fill):
                        result = self.check_points(corners, canvas, .125)
                        projected = [((p[0] * f32((16 / 9) / (16 / 10)) + 1) * canvas[0] / 2,
                                      (1 - p[1]) * canvas[1] / 2) for p in result]
                        for old, new in zip(corners, projected):
                            self.assertAlmostEqual(new[0], f32(old[0]), delta=.002)
                            self.assertAlmostEqual(new[1], f32(old[1]), delta=.002)
                        self.assertAlmostEqual(projected[1][0] - projected[0][0], 10 * scale, delta=.002)

    def test_relative_factor_loads_and_exit_survive_whole_block_relocation(self):
        points = [(142.22, 181, math.nan, math.nan), (900.75, 181, math.nan, math.nan),
                  (1639, 181, math.nan, math.nan)]
        expected = self.check_points(points, (1920, 1080), .25)
        for address in (0x10000, 0x12345678, 0x7ff0000000):
            with self.subTest(address=address):
                self.assertEqual(self.check_points(points, (1920, 1080), .25, address), expected)

    def test_interpreter_rejects_out_of_bounds_point_store_and_exit(self):
        # Exercise the interpreter's bounds contract rather than letting a wrong
        # encoded third corner overwrite an adjacent native stack allocation.
        bad_store = bytearray(self.code)
        store = bytes.fromhex('c5 f8 11 42 20')
        self.assertEqual(bad_store.count(store), 1)
        bad_store[bad_store.index(store) + 4] = 0x30
        bad_exit = bytearray(self.code)
        jump = self.code.index(b'\xeb\x36')
        bad_exit[jump + 1] = 0x37
        for code in (bad_store, bad_exit):
            with self.subTest(code=bytes(code).hex()):
                with self.assertRaisesRegex(AssertionError, 'outside|exactly'):
                    VfxMachine(code, [(10, 20, 0, 1)] * 3, (1920, 1080), .25).run()


if __name__ == '__main__':
    unittest.main()
