"""Scoped online widescreen corrections. Author: tcosi.
SPDX-License-Identifier: GPL-2.0-or-later
"""
import math
import struct
import xml.etree.ElementTree as ET

from widescreen_startup import IGZ

NOTICE = 'ui/OnlineInvitation_2015.igz'
REPORT = 'ui/Octane_AAR_GrandPrixReport.igz'
CHALLENGES = 'ui/Octane_GrandPrix_Persistent.igz'
ICON_TEXT_CLEARANCE = 80.0  # 64-unit icon half-width plus 16 units of padding


def correct_notice(data, factor):
    """Bring independently rendered notification text into the centered sprite layout."""
    ig = IGZ(data)
    result = bytearray(data)
    by_ptr = {o['ptr']: o for o in ig.objects}
    found = set()
    names = {'Text_FriendsMessageFont', 'Text_FriendsMessageFont001',
             'Text_StartFont', 'Text_StartFont001'}
    for o in ig.objects:
        if o['type'] != 'igGuiPlaceable' or o.get('name') not in names:
            continue
        at = o['offset']
        parent = by_ptr[ig.q(at + 0x40)]
        if parent.get('name') != 'Sprite_Friends_HUDOnlineInvitation_tga':
            raise ValueError('Unexpected notification parent')
        po = parent['offset']
        px = struct.unpack_from('<f', data, po + 0x60)[0]
        ps = struct.unpack_from('<f', data, po + 0x84)[0]
        x = struct.unpack_from('<f', data, at + 0x60)[0]
        sx = struct.unpack_from('<f', data, at + 0x84)[0]
        if ps != .75 or not math.isfinite(x) or not 0 < sx < 2:
            raise ValueError('Unexpected notification transform')
        struct.pack_into('<f', result, at + 0x60,
                         (x + ICON_TEXT_CLEARANCE) * factor + (960 - px) * (1 - factor) / ps)
        struct.pack_into('<f', result, at + 0x84, sx * factor)
        found.add(o['name'])
    if found != names:
        raise ValueError('Notification text placeables missing')
    return bytes(result)


def _native_blue_bounds(data, factor, name):
    """Use native asset bindings and explicit bounds, as in the existing menu fixes."""
    ig = IGZ(data)
    result = bytearray(data)
    found = 0
    for obj in ig.objects:
        if obj['type'] != 'igGuiPlaceable' or obj.get('name') != name:
            continue
        at = obj['offset']
        x = struct.unpack_from('<f', data, at + 0x60)[0]
        if not math.isfinite(x):
            raise ValueError('Unexpected background position')
        flags = struct.unpack_from('<I', data, at + 12)[0]
        struct.pack_into('<I', result, at + 12, flags & ~1)
        struct.pack_into('<f', result, at + 0x84, 1)
        width = 1920 / factor + 2 * abs(x - 960) + 128
        # Extend the left side only: the native angled edge must stay outside the
        # viewport even at its widest (bottom) point. Preserve the right-hand bound.
        left_overscan = (1920 / factor) * .4 + 128
        struct.pack_into('<f', result, at + 0x60, 960 - left_overscan)
        width += 2 * left_overscan
        struct.pack_into('<f', result, at + 0x90, width)
        found += 1
    if found != 1:
        raise ValueError('Native blue background missing: ' + name)
    return bytes(result)


def correct_report(data, factor):
    # The post-standings report includes Position Rewards and Challenge Completed.
    # WumpaOnly is a different layout; keep its component and standings native.
    return _native_blue_bounds(data, factor, 'Sprite_octane_ui_menu_bg_tga')


def correct_challenges(data, factor):
    return _native_blue_bounds(data, factor, 'Sprite_octane_ui_menu_bg_tga')


def correct_markers(xml, factor):
    """Undo centered-HUD contraction of camera-projected marker positions (USA).

    The CPU camera already projects into the wide viewport. Expand the distance from
    its center before the centered HUD contracts it, keeping labels over their racers.
    """
    lines = [l for l in xml.iter('Line') if int(l.get('Address'), 0) == 0x67d4e5]
    if len(lines) != 1:
        raise ValueError('Expected widescreen GUI patch missing')
    line = lines[0]
    blob = bytearray.fromhex(line.get('Value'))
    # This space was freed by the existing AVX -> SSE rewrite. Never overwrite live code.
    if factor != .5:
        # 21:9 keeps a 20-byte constant pool in the middle of the freed space.
        # Move it to the end and re-aim its four existing RIP-relative users.
        pool = 0x67d7b0 - 0x67d4e5
        if blob[pool + 20:] != b'\x90' * 32:
            raise ValueError('Unexpected 21:9 constant pool')
        constants = blob[pool:pool + 20]
        blob[pool:pool + 32] = b'\x90' * 32
        blob[pool + 32:pool + 52] = constants
        for address, end in ((0x7265d8, 0x7265f8), (0x7265d8, 0x726618),
                             (0x67d4e5, 0x67d749), (0x658df9, 0x658e23)):
            target_line = next(l for l in xml.iter('Line') if int(l.get('Address'), 0) == address)
            target_blob = blob if target_line is line else bytearray.fromhex(target_line.get('Value'))
            field = end - address - 4
            old = end + struct.unpack_from('<i', target_blob, field)[0]
            if old not in (0x67d7b0, 0x67d7c0):
                raise ValueError('Unexpected 21:9 pool reference')
            struct.pack_into('<i', target_blob, field, old + 32 - end)
            if target_line is not line:
                target_line.set('Value', target_blob.hex())
    start = 0x67d780
    pos = start - 0x67d4e5
    def call(at, target):
        return b'\xe8' + struct.pack('<i', target - at - 5)
    # XML addresses include the launcher's 0x400000 patch base.
    code = bytearray(bytes.fromhex('4883ec08') + call(start + 4, 0x10bd820) + bytes.fromhex('4883c408'))
    code += bytes.fromhex('f30f1045a8 f30f108bbc020000 f30f588bc0020000')
    # Center comes from the actual camera viewport, supporting internal resolutions.
    constants = []
    for index, (op, value) in enumerate([('f30f590d', .5), ('f30f5905', 1 / factor)]):
        if index == 1:
            code += bytes.fromhex('f30f5cc1')
        at = len(code)
        code += bytes.fromhex(op) + b'\0' * 4
        constants.append((at + 4, value))
    code += bytes.fromhex('f30f58c1 f30f1145a8 c3')
    for field, value in constants:
        struct.pack_into('<i', code, field, len(code) - (field + 4))
        code += struct.pack('<f', value)
    if pos < 0 or len(blob) - pos < len(code) or any(b != 0x90 for b in blob[pos:pos+len(code)]):
        raise ValueError('No reserved space for kart-marker correction')
    blob[pos:pos+len(code)] = code
    line.set('Value', blob.hex())
    patch_list = next(xml.iter('PatchList'))
    ET.SubElement(patch_list, 'Line', Type='bytes', Address='0xdd7853',
                  Value=call(0xdd7853, start).hex(), Signature='e8 c8 5f 2e 00')
