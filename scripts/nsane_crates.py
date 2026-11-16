"""N.Sane Mode's extra item crates: the view of update2.pak that private N.Sane races read.

A row of 4 crates gets 2 more in front of it (in the race direction, level with the gaps), a row of 3 gets 3 and
a row of 2 gets 2; where in front would leave the track they go nearer the middle, behind, or in the row. The
originals do not move. Crates are map entities in maps/Octane/<track>/<track>_(Power_Up|Design).igz: CGameEntity
(+0x18 igComponentList, +0x20 position, +0x30 igEntityTransform, +0x40 data), with igObjectList and igNameList
side by side. The race direction comes from the CRaceTrackSplineComponentData splines. The Grand Prix tracks are
archives nested in update2.pak and are not changed: there src/runtime_nsane.c gives both items from one crate.
"""
import bisect
import itertools
import math
import re
import struct
from pathlib import Path

from archive_reader import Archive
from igz_file import Igz, unpack_offsets

FRENTE = 600.0                                # distancia de las 2 nuevas delante de la fila
# Pistas donde delante de las filas el suelo sube (rampas, escalones) y las nuevas salian enterradas: Papu's Pyramid
# (primeras cajas), N. Gin Labs (espiral cuadrada) y Barin Ruins (curva del robot). Ahi las nuevas van EN la fila,
# entre las originales, sobre el mismo suelo que ellas.
EN_FILA = {'t133_papus_pyramid', 't151_ngin_labs', 't222_barin_ruins'}
MAX_GAP = 2500.0                              # hueco maximo entre cajas vecinas de una fila
ITEM = re.compile(r'question|powerupcrate|^crate(_?\d*)$', re.I)

def fnv(s):
    h = 0x811c9dc5
    for c in s.lower().encode():
        h = ((h ^ c) * 0x01000193) & 0xffffffff
    return h

def race_paths(blobs):
    """Curvas de carrera en coordenadas del mundo: [[(x, y, z), ...], ...] en sentido de carrera."""
    pos_paths, other = [], []
    for blob in blobs:
        g = Igz(blob)
        objs = g.objects(); kind = dict(objs); starts = sorted(kind); rofs = g.sites['ROFS']
        def reach(o, seen):
            if o in seen or o not in kind: return seen
            seen.add(o)
            i = bisect.bisect_left(rofs, o); j = bisect.bisect_right(starts, o)
            end = starts[j] if j < len(starts) else len(g.data)
            while i < len(rofs) and rofs[i] < end:
                reach(g.u64(rofs[i]), seen); i += 1
            return seen
        nls = [o for o, k in objs if k == 'igNameList']
        if not nls: continue
        nl = nls[0]; root, names = g.u64(0x20), g.u64(nl + 0x20)
        for i in range(g.u32(nl + 0x0c)):
            o = g.u64(root + 8 * i)
            if kind.get(o) not in ('CEntity', 'CGameEntity'): continue
            seen = reach(o, set())
            if not any(kind[x] == 'CRaceTrackSplineComponentData' for x in seen): continue
            pos = struct.unpack_from('<3f', g.data, o + 0x20)
            for lst in [x for x in seen if kind[x] == 'igSplineControlPoint2List']:
                arr = g.u64(lst + 0x20)
                pts = [tuple(a + b for a, b in zip(struct.unpack_from('<3f', g.data, g.u64(arr + 8 * k) + 0x0c), pos))
                       for k in range(g.u32(lst + 0x0c))]
                (pos_paths if g.strings[g.u64(names + 16 * i)].startswith('Position') else other).append(pts)
    res = Paths(pos_paths + other); res.npos = len(pos_paths)   # las de posicion no pasan por cada carril
    return res

class Paths(list):
    npos = 0                                  # las primeras npos son Position_Spline

def nearest(paths, c):
    best = None
    for pts in paths:
        for a, b in zip(pts, pts[1:]):
            d = [b[i] - a[i] for i in range(3)]
            f = max(0.0, min(1.0, sum((c[i] - a[i]) * d[i] for i in range(3)) / (sum(v * v for v in d) or 1.0)))
            dist = math.dist(c, [a[i] + d[i] * f for i in range(3)])
            if best is None or dist < best[0]: best = (dist, d)
    return best

def forward_at(paths, c):
    """Direccion (dx, dy, dz) de la curva junto a c y la distancia: Position_Spline si pasa cerca; si no, cualquiera."""
    best = nearest(paths[:paths.npos], c)
    if best is None or best[0] > 1500:
        best = nearest(paths, c)
    return best[1], best[0]

def sort_row(quad):
    p = [c[1] for c in quad]
    ax = max(range(2), key=lambda a: max(q[a] for q in p) - min(q[a] for q in p))
    return sorted(quad, key=lambda c: c[1][ax])

def clusters(cs, link):
    left, res = list(cs), []
    while left:
        grp, changed = [left.pop(0)], True
        while changed:
            changed = False
            for c in left[:]:
                if any(math.dist(c[1], g[1]) < link for g in grp):
                    grp.append(c); left.remove(c); changed = True
        res.append(grp)
    return res

def find_rows(cs, paths):
    """Grupos de 4 cajas en fila. cs = [(off, (x, y, z))]."""
    rows = [sort_row(g) for g in clusters(cs, 900.0) if len(g) == 4]
    used = {c[0] for r in rows for c in r}
    rest = [c for c in cs if c[0] not in used]
    cand = []
    for quad in itertools.combinations(rest, 4):
        order = sort_row(quad); q = [c[1] for c in order]
        ux, uy = q[3][0] - q[0][0], q[3][1] - q[0][1]
        n = math.hypot(ux, uy)
        if n < 300 or n > 3 * MAX_GAP: continue
        off = max(abs((v[0] - q[0][0]) * uy - (v[1] - q[0][1]) * ux) / n for v in q)   # distancia a la recta
        gaps = [math.dist(a, b) for a, b in zip(q, q[1:])]
        if off > 0.15 * n or max(gaps) > MAX_GAP or max(gaps) > 2.2 * min(gaps): continue
        mid = tuple(sum(v[i] for v in q) / 4 for i in range(3))
        d, dist = forward_at(paths, mid)
        h = math.hypot(d[0], d[1]) or 1.0
        if abs(ux * d[0] + uy * d[1]) / (n * h) > 0.7 or dist > 3000: continue         # debe cruzar la pista
        cand.append((off / n + max(gaps) / min(gaps) * 0.01, order))
    for _, order in sorted(cand, key=lambda c: c[0]):
        if used.isdisjoint(c[0] for c in order):
            rows.append(order); used |= {c[0] for c in order}
    # v0.4: lo que quedo: filas de 4 con un hueco grande en medio, de 3 y de 2
    for size, ratio, gap in ((4, 3.5, MAX_GAP), (3, 2.2, MAX_GAP), (2, 1.0, 1600.0)):
        rest = [c for c in cs if c[0] not in used]
        cand = []
        for grp in itertools.combinations(rest, size):
            order = sort_row(grp); q = [c[1] for c in order]
            ux, uy = q[-1][0] - q[0][0], q[-1][1] - q[0][1]
            n = math.hypot(ux, uy)
            gaps = [math.dist(a, b) for a, b in zip(q, q[1:])]
            if n < 300 or max(gaps) > gap or min(gaps) < 250 or max(gaps) > ratio * min(gaps): continue
            if abs(q[-1][2] - q[0][2]) > 0.5 * n: continue
            off = max(abs((v[0] - q[0][0]) * uy - (v[1] - q[0][1]) * ux) / n for v in q)
            if off > 0.15 * n: continue
            mid = tuple(sum(v[i] for v in q) / size for i in range(3))
            d, dist = forward_at(paths, mid)
            h = math.hypot(d[0], d[1]) or 1.0
            if abs(ux * d[0] + uy * d[1]) / (n * h) > 0.7 or dist > 5000: continue
            cand.append((off / n + max(gaps) / min(gaps) * 0.01, order))
        for _, order in sorted(cand, key=lambda c: c[0]):
            if used.isdisjoint(c[0] for c in order):
                rows.append(order); used |= {c[0] for c in order}
    return rows

def patch(blob, paths, en_fila=False):
    g = Igz(blob)
    for tag in ('RHND', 'RNEX'):
        if tag not in g.sites and any(f[0] == tag for f in g.fixups):
            g.sites[tag] = g.read_list(tag, unpack_offsets)
    root = g.read_list('ROOT', unpack_offsets)[0]
    names_list, = [off for off, kind in g.objects() if kind == 'igNameList']
    n = g.u32(root + 0x0c)
    assert g.u32(names_list + 0x0c) == n
    arr, narr = g.u64(root + 0x20), g.u64(names_list + 0x20)
    objs = [g.u64(arr + 8 * i) for i in range(n)]
    names = [(g.u64(narr + 16 * i), g.u64(narr + 16 * i + 8)) for i in range(n)]
    for idx, h in names:                      # el hash de los nombres es el que creemos
        assert fnv(g.strings[idx]) == h & 0xffffffff, (g.strings[idx], hex(h), hex(fnv(g.strings[idx])))
    cs = []
    for off, (idx, _) in zip(objs, names):
        if g.types[g.u64(off)] == 'CGameEntity' and ITEM.search(g.strings[idx]) and not g.strings[idx].endswith('_gen'):
            cs.append((off, struct.unpack_from('<3f', g.data, off + 0x20)))
    size = lambda off: g.sizes[g.u64(off)]

    def clone(off):
        new = g.append(bytes(g.data[off:off + size(off)]), 16)   # entidad y transform van a 16 (vectores)
        for tag, sites in g.sites.items():
            for site in [s for s in sites if off <= s < off + size(off)]:
                g.add_site(tag, new + site - off)
        return new

    rows = find_rows(cs, paths)
    k, far = 0, 0.0
    moved = [0, 0, 0]; sizes = {}
    for r in rows:
        pts = [c[1] for c in r]
        L = len(pts)
        sizes[L] = sizes.get(L, 0) + 1
        def at(t):                            # punto t (0..L-1) sobre las cajas originales: sigue el suelo
            i = min(int(t), L - 2); f = t - i
            return tuple(pts[i][a] + (pts[i + 1][a] - pts[i][a]) * f for a in range(3))
        d, _ = forward_at(paths, at((L - 1) / 2))
        ux, uy = pts[-1][0] - pts[0][0], pts[-1][1] - pts[0][1]
        n = math.hypot(ux, uy); nx, ny = -uy / n, ux / n     # perpendicular a la fila, en el suelo
        if nx * d[0] + ny * d[1] < 0: nx, ny = -nx, -ny       # hacia donde va la carrera
        # cajas a la misma altura exacta = flotando o suelo plano: las nuevas a esa misma altura
        slope = 0.0 if max(v[2] for v in pts) - min(v[2] for v in pts) < 1 else d[2] / (math.hypot(d[0], d[1]) or 1.0)
        place = lambda tt, s=1.0: (lambda p: (p[0] + s * nx * FRENTE, p[1] + s * ny * FRENTE, p[2] + s * slope * FRENTE))(at(tt))
        limit = max(nearest(paths, v)[0] for v in pts) + 250
        ok = lambda spots, s: max(nearest(paths, place(q, s))[0] for q in spots) <= limit
        front = {4: (0.5, 2.5), 3: (0.0, 1.0, 2.0), 2: (0.0, 1.0)}[L]
        closer = {4: (1.0, 2.0), 3: (0.25, 1.0, 1.75), 2: (0.25, 0.75)}[L]
        if en_fila: where = [at(q) for q in {4: (0.5, 2.5), 3: (0.5, 1.5), 2: (1 / 3, 2 / 3)}[L]]
        elif ok(front, 1.0): where = [place(q) for q in front]
        elif ok(closer, 1.0): where = [place(q) for q in closer]; moved[0] += 1
        elif ok(front, -1.0): where = [place(q, -1.0) for q in front]; moved[1] += 1     # atras
        else: where = [at(q + 0.5) for q in range(L - 1)][:2]; moved[2] += 1              # en la misma fila
        for pos in where:
            far = max(far, nearest(paths, pos)[0])
            src = r[1][0]
            e = clone(src)
            lst, tr = clone(g.u64(src + 0x18)), clone(g.u64(src + 0x30))
            g.set_u64(e + 0x18, lst); g.set_u64(e + 0x30, tr)
            struct.pack_into('<3f', g.data, e + 0x20, *pos)
            k += 1
            name = f'crate_question_x{k:03d}'
            objs.append(e); names.append((g.string_index(name), fnv(name)))
    if not rows:
        return None, 0, 0, len(cs), far, moved, sizes
    m = len(objs)

    def relist(lst, width, raw, sites):
        new = g.append(raw)
        g.set_u32(lst + 0x0c, m); g.set_u32(lst + 0x10, m); g.set_u32(lst + 0x18, width * m)
        g.set_u64(lst + 0x20, new)
        for s in sites(new):
            g.add_site(*s)

    relist(root, 8, b''.join(struct.pack('<Q', o) for o in objs),
           lambda a: [('ROFS', a + 8 * i) for i in range(m)])
    relist(names_list, 16, b''.join(struct.pack('<QQ', *x) for x in names),
           lambda a: [('RSTT', a + 16 * i) for i in range(m)])
    return g.build(), len(rows), k, len(cs), far, moved, sizes


# Files that hold neither crates nor race splines: skipped so the first launch reads less.
NOT_RACE_DATA = re.compile(r'_art_|_lighting|_audio|_vfx|occlusion|_animation|staticcollision|_minimap|_zoneinfo', re.I)
LAYER = re.compile(r'^maps/octane/(t\d{3}_[^/]+)/[^/]+\.igz$')


def update2_entries(game):
    """{index in update2.pak: (original bytes, bytes with the extra crates)}."""
    folder = Path(game) / 'archives'
    update2 = Archive(folder / 'update2.pak')
    opened = [update2]
    try:
        update = folder / 'update.pak'
        layers = [Archive(update)] if update.is_file() else []
        opened += layers
        stems = sorted({m.group(1) for f in update2.files if (m := LAYER.match(f['path'].lower()))})
        result = {}
        for stem in stems:
            pak = folder / f'{stem}.pak'
            base = [Archive(pak)] if pak.is_file() else []
            try:
                assets = {}
                for archive in base + layers + [update2]:     # the last archive that has a file wins
                    for item in archive.files:
                        path = item['path'].lower()
                        if path.startswith(f'maps/octane/{stem}/') and path.endswith('.igz') \
                                and not NOT_RACE_DATA.search(path):
                            assets[path] = (archive, item)
                blobs = {path: archive.extract(item) for path, (archive, item) in assets.items()}
                paths = race_paths([b for b in blobs.values() if b'CRaceTrackSplineComponentData' in b])
                if not paths:
                    continue
                for path, blob in blobs.items():
                    archive, item = assets[path]
                    if archive is not update2 or b'CCollectibleCrateComponentData' not in blob \
                            or b'igNameList' not in blob:
                        continue
                    changed = patch(blob, paths, stem in EN_FILA)[0]
                    if changed:
                        result[item['index']] = (blob, changed)
            finally:
                for archive in base:
                    archive.data.close()
                    archive._file.close()
        return result
    finally:
        for archive in opened:
            archive.data.close()
            archive._file.close()
