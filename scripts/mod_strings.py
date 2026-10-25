"""localized_strings operations: texts of an IGZ's igLocalizedStringData objects (the game's
looseData/systemData_<language>.lng), so that several packages can each change their own texts
in the same archive entry.

An igLocalizedStringData object holds {+0x10 TSTR index of the text, +0x18 handle of the object
it belongs to, +0x20 handle of the field}. EXNM lists the object names and the field names
(namespace "metafield") together; each handle is an index in its own kind's list.
SPDX-License-Identifier: GPL-2.0-or-later
"""
import struct
from igz_file import Igz


def texts(z):
    """{(object, field): offset of the object} of the igLocalizedStringData objects of z."""
    f = z.fixup('EXNM')
    named = [struct.unpack_from('<II', f[3], 8 * i) for i in range(f[1])]
    named = [(z.strings[a & 0x7FFFFFFF], z.strings[b & 0x7FFFFFFF]) for a, b in named]
    objects = [n for n, space in named if space != 'metafield']
    fields = [n for n, space in named if space == 'metafield']
    return {(objects[z.u64(off + 0x18) & 0x7FFFFFFF], fields[z.u64(off + 0x20) & 0x7FFFFFFF]): off
            for off, kind in z.objects() if kind == 'igLocalizedStringData'}


def apply(blob, changes):
    """The IGZ again with the texts {(object, field): text}; each must already exist."""
    z = Igz(blob)
    found = texts(z)
    for key, text in sorted(changes.items()):
        if key not in found:
            raise ValueError('No localized string %s %s' % key)
        z.set_u64(found[key] + 0x10, z.string_index(text))
    return z.build()
