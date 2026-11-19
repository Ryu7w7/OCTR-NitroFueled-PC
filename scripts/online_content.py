#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""What a launch plays online with, for public matchmaking and Ranked (private rooms take anything):

  clean       the game's own files and the port's own patches
  characters  plus mods that only add racers, outfits and skins (checked file by file)
  modified    anything else: the player's own executable patches, mods that change items, tracks,
              karts, physics or game rules, or a character mod that carries such a change

The game sends the class to RyuNET (BB_ONLINE_CONTENT, src/runtime_http.c X-RyuNET-Content) and refuses
public matchmaking and Ranked itself when it is "modified" (src/runtime_public_itemless.c).

How a character mod is told apart (checked 2026-11-18 on eight GameBanana racers: CJ, Donald, Luigi,
Mario, Miku, Shrek, Sonic, SpongeBob):
  - its archives' entries are new files, or byte-identical copies of the game's, or visual files
    (textures, materials, models, outfits, animations, sounds, effects) even when they replace the
    game's; a game script (vsc/) only when identical or in VISUAL_SCRIPTS;
  - its looseData/systemData.igz (every racer mod carries a whole one) only ADDS objects of
    ADDED_TYPES; every other object is the game's own, compared by content and by what its
    references point at, never by position (CONTAINERS, which list the new objects, may grow).
    Driver stats, kart physics, boosts, item odds, AI difficulty live in other types, so a mod that
    changes them is "modified" (tested: boost power, global kart data, max speed, AI difficulty);
  - texts (.lng) and the character listing only name and list racers.
A modified game can still claim anything: this keeps out mods and patches, not a rebuilt client.

Usage (run.py and run.sh, after scripts/patches.py; prints class=<...> then the reasons):
  online_content.py --game <the game folder> --view <the launch's game view> --out <out>
      [--file-map <BB_MOD_FILE_MAP>] [--archive-plan <BB_MOD_ARCHIVE_PLAN>] [--layout-dir <dir>]
"""
import argparse
import hashlib
import json
import os
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

from archive_reader import Archive

CLEAN, CHARACTERS, MODIFIED = 'clean', 'characters', 'modified'

ADDED_TYPES = {'CRacerDriverData', 'CDriverSkinData', 'CDriverSkinDataList', 'COutfitData',
               'COutfitMaterialPackage', 'COutfitMaterialPackageList'}
CONTAINERS = {'CRacerDriverDataList', 'CSystemData', 'igNameList', 'igObjectList'}
SYSTEM_DATA = 'loosedata/systemdata.igz'
# Folders of visual files: a character mod may replace the game's own there too (Miku replaces the
# trophy girls' textures, Mario and Luigi the shared hurt effects and the small drivers' animations).
VISUAL = ('textures/', 'materialinstances/', 'actors/', 'actors_stripped/', 'outfits/', 'characters/',
          'packages/generated', 'anims/', 'behaviors/drivers/', 'behavior_events/drivers/',
          'sound_samples/', 'sound_streams/', 'sounds/', 'vfx/', 'models/', 'fonts/',   # sound: custom music too
          'ui/')        # HUDs: the screens' layouts and pictures (their scripts, vsc/ui/, only when approved)
# Game scripts that character mods change and that only draw effects (read: what a hit looks like)
VISUAL_SCRIPTS = {'vsc/common/common_driver_victimfx.igz'}
TEXTS = ('loosedata/systemdata_',)          # .lng: names of the new racers
LISTINGS = {'characterlisting.igz'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


# ---- the port's own executable patches ----
# Patches of the package's patches/ folder that public play allows, by content: the player's own XML
# files live in the same folder on Windows, and a bundled file can be edited. Generated from the
# repository's patches/*.xml by patch_digests() (a name per game version: EU has two);
# tests/test_online_content.py keeps it current.
TRUSTED_PATCHES = {
    'CrashTeamRacing.xml/60 FPS Unlock': {'73cc120f4199'},
    'CrashTeamRacing.xml/Disable Motion Blur': {'1afd4b7cfd53'},
    'CrashTeamRacing.xml/Resolution Patch (1080p for PS4 Pro)': {'929be3d2d320'},
    'CrashTeamRacingEU.xml/60 FPS Unlock': {'bdc816858a4f', 'e1c58d7d0673'},
    'CrashTeamRacingEU.xml/Disable Motion Blur': {'1afd4b7cfd53', 'e2fda5c1a8f5'},
    'CrashTeamRacingEU.xml/Resolution Patch (1080p for PS4 Pro)': {'6cfa8a868b22', '70b234720583'},
    'CrashTeamRacingOnline.xml/Online: custom Demonware key': {'82b3ba109feb'},
    'CrashTeamRacingOnlineEU.xml/Online: custom Demonware key': {'e94f7ec0be7a'},
    'CrashTeamRacingUnlockAll.xml/Unlock Everything': {'043039356707'},
    'CrashTeamRacingUnlockAllEU.xml/Unlock Everything': {'f7b9af6efb8f'},
}


# Mods' executable patches reviewed and allowed in public play, by content: the exact code reviewed,
# whatever the file or the patch is called (a changed byte is another digest, "modified").
APPROVED_MOD_PATCHES = {
    '54f024bf4952': 'Input Display 1.0.1 (Onur): draws the pressed buttons, USA 1.21',
    # the same code for EU 1.21 (tools/port_patch_eu.py; its one call into the game moves 0x149fca0 -> 0x149fc90)
    'dc2025aa9102': 'Input Display 1.0.1 (Onur): draws the pressed buttons, EU 1.21',
}


# The patches scripts/patches.py writes itself (render_plan_patches, theme_patches, end_race_patches)
PORT_PATCHES = ('render/', 'theme/', 'end_race/')


def patch_digest(meta, value=True):
    """The content of one <Metadata> patch: its lines' writes, not its name or formatting (value=False:
    where it writes, not what: the online key patch's shape)"""
    keys = ('Type', 'Address', 'Value', 'Signature', 'SignatureOffset', 'ExpectedSHA256')
    lines = []
    for line in meta.iter('Line'):
        lines.append('|'.join(str(line.get(k, '')).strip().lower() if value or k != 'Value' else ''
                              for k in keys))
    return sha('\n'.join(lines).encode())[:12]


# scripts/online_key_local.py rewrites the online key patch with another server's public key (a local
# or private RyuNET): the same single write, another key. By its shape (patch_digest value=False).
ONLINE_KEY_PATCHES = {
    'CrashTeamRacingOnline.xml/Online: custom Demonware key': '6887a6f03ec0',
    'CrashTeamRacingOnlineEU.xml/Online: custom Demonware key': 'e6ff92e1caf2',
}


def patch_digests(folder):
    """{key: digest} of every patch of folder/*.xml (all versions: the key is file/name per AppVer)"""
    out = {}
    for path in sorted(Path(folder).glob('*.xml')):
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError:
            continue
        for meta in root.iter('Metadata'):
            out.setdefault(f'{path.name}/{meta.get("Name")}', set()).add(patch_digest(meta))
    return out


def patch_reasons(applied, patch_dirs, layout_dir, app_version, title_id):
    """Reasons the applied patches (out/patches-applied.json keys) are not the port's own"""
    import patches
    # the patch each key was applied from: this game version's, the later folder's (as patches.py)
    used = {key: meta for key, _, meta in patches.all_external_patches(patch_dirs, app_version, title_id)}
    generated = set(patch_digests(layout_dir)) if layout_dir else set()
    reasons = []
    for key in applied:
        # made by the port itself (patches.py: the internal resolution or DLSS render plan, the title
        # theme, the online race end; the aspect ratio's layout) or Pack Tacho (verified)
        if key.startswith(PORT_PATCHES) or key in generated or patches.tacho_pack_patch(key):
            continue
        meta = used.get(key)
        if meta is not None and not any(True for _ in meta.iter('Line')):
            continue        # writes nothing: the mod library's switch of a package (OCTR-Mod-Library.xml)
        if meta is not None and (patch_digest(meta) in TRUSTED_PATCHES.get(key, set()) or
                                 patch_digest(meta) in APPROVED_MOD_PATCHES or
                                 patch_digest(meta, value=False) == ONLINE_KEY_PATCHES.get(key)):
            continue
        reasons.append(f'executable patch "{key}"')
    return reasons


# ---- the game's files ----
class GameIndex:
    """Where each entry path of the game's archives is, read lazily: its copies' digests."""

    def __init__(self, game):
        self.archives = {}
        self.where = {}
        folder = Path(game) / 'archives'
        for path in sorted(folder.glob('*.pak')) if folder.is_dir() else []:
            try:
                archive = Archive(path)
            except ValueError:
                continue
            self.archives[path.name.lower()] = archive
            for f in archive.files:
                self.where.setdefault(f['path'].lower(), []).append((archive, f))
        self._digests = {}

    def has(self, path):
        return path.lower() in self.where

    def digests(self, path):
        key = path.lower()
        if key not in self._digests:
            self._digests[key] = {sha(a.extract(f)) for a, f in self.where.get(key, [])}
        return self._digests[key]

    def system_data(self):
        """The game's own systemData.igz: the last update archive's (update2 in 1.21)"""
        for name in sorted(self.archives, reverse=True):
            if name.startswith('update'):
                for f in self.archives[name].files:
                    if f['path'].lower() == SYSTEM_DATA:
                        return self.archives[name].extract(f)
        for archive, f in self.where.get(SYSTEM_DATA, []):
            return archive.extract(f)
        return None


# ---- systemData: only racers, outfits and skins added ----
class _Canon:
    """Objects of an IGZ with their content: bytes, strings by text, references by what they point at
    (the target's type and its own content, two levels deep), memory blocks by their content."""

    def __init__(self, blob):
        from bisect import bisect_right
        from igz_file import Igz
        self._bisect = bisect_right
        self.igz = g = Igz(blob)
        self.objs = g.objects()
        self.size_of = {t: g.sizes[i] for i, t in enumerate(g.types)}
        self.refs = set(g.sites.get('ROFS', []))
        self.strs = set(g.sites.get('RSTT', []))
        self.mems = set(g.sites.get('RPID', []))
        self.type_at = dict(self.objs)
        bounds = {o for o, _ in self.objs}
        bounds.update(g.u64(s) & 0x7fffffffffffffff for s in self.mems)
        self.bounds = sorted(bounds) + [len(g.data)]
        self.memo = {}

    def _block(self, start):
        i = self._bisect(self.bounds, start)
        return start, self.bounds[i] if i < len(self.bounds) else len(self.igz.data)

    def _flat(self, start, end, depth):
        g, out, o = self.igz, [], start
        while o < end:
            if o in self.strs:
                i = g.u64(o)
                out.append(b'S' + (g.strings[i].encode() if i < len(g.strings) else b'?'))
                o += 8
            elif o in self.refs:
                t = self.type_at.get(g.u64(o))
                out.append(b'R?' if t is None else b'R' + t.encode() + (self.sig(g.u64(o), depth - 1) if depth > 0 else b''))
                o += 8
            elif o in self.mems:
                t = g.u64(o) & 0x7fffffffffffffff
                if t == 0:
                    out.append(b'M0')           # a null block (an empty table)
                elif depth > 0:
                    out.append(b'M' + hashlib.sha1(self._flat(*self._block(t), depth - 1)).digest())
                else:
                    out.append(b'M')
                o += 8
            else:
                out.append(bytes(g.data[o:o + 4]))
                o += 4
        return b''.join(out)

    def sig(self, off, depth=2):
        key = (off, depth)
        if key not in self.memo:
            self.memo[key] = b''                # a reference cycle reads as empty
            self.memo[key] = hashlib.sha1(self._flat(off, off + self.size_of[self.type_at[off]], depth)).digest()
        return self.memo[key]

    def by_type(self):
        from collections import Counter, defaultdict
        out = defaultdict(Counter)
        for off, t in self.objs:
            out[t][self.sig(off)] += 1
        return out


def system_data_reasons(mod_blob, game_blob):
    """Why a mod's systemData.igz is more than new racers, outfits and skins ([] when it is not)"""
    if game_blob is None:
        return ['its systemData has no game copy to compare with']
    try:
        game, mod = _Canon(game_blob).by_type(), _Canon(mod_blob).by_type()
    except (ValueError, KeyError, IndexError, struct.error) as error:
        return [f'its systemData cannot be read ({error})']
    changed = []
    for t in sorted(set(game) | set(mod)):
        if t in CONTAINERS:
            continue
        if game[t] - mod[t] or (mod[t] - game[t] and t not in ADDED_TYPES):
            changed.append(t)
    return [f'its systemData changes {", ".join(changed[:6])}{"..." if len(changed) > 6 else ""}'] if changed else []


# ---- one entry, one archive ----
def entry_reasons(path, data, game):
    """Why one file a mod gives the game is not a character mod's ([] when it is)"""
    key = path.lower().replace('\\', '/')
    if game.has(key) and sha(data) in game.digests(key):
        return []                               # the game's own file, copied along
    if key == SYSTEM_DATA:
        return system_data_reasons(data, game.system_data())
    if key in LISTINGS or (key.startswith(TEXTS) and key.endswith('.lng')):
        return []
    if key.startswith('vsc/'):
        return [] if key in VISUAL_SCRIPTS else [f'it changes the game script {path}']
    if key.startswith(VISUAL):
        return []
    return [f'it changes {path}' if game.has(key) else f'it adds {path}']


def archive_reasons(path, game, label):
    try:
        archive = Archive(path)
    except (ValueError, OSError) as error:
        return [f'{label}: cannot be read ({error})']
    out = []
    for f in archive.files:
        for reason in entry_reasons(f['path'], archive.extract(f), game):
            out.append(f'{label}: {reason}')
            if len(out) >= 4:
                return out
    return out


def read_file_map(path):
    """[(guest relative path, source file)] of a BBFILE01 map (scripts/mod_overlays.py file_map)"""
    data = Path(path).read_bytes()
    if data[:8] != b'BBFILE01':
        raise ValueError('not a mod file map')
    count, = struct.unpack_from('<I', data, 8)
    out, p = [], 12
    for _ in range(count):
        a, b = struct.unpack_from('<II', data, p)
        p += 8
        out.append((data[p:p + a].decode(), data[p + a:p + a + b].decode()))
        p += a + b
    return out


def mod_reasons(game_dir, view_dir, file_map, archive_plan, own_dir=None):
    """Why the launch's mods are more than character mods; (reasons, number of mod files looked at).
    own_dir: the launch's out folder; files the port makes there itself (scripts/online_mode.py's
    menus, through the same file map as mods: out/online-mode/cup-files.map) are not mods."""
    # The archive reader follows the launch's mod file map (BB_MOD_FILE_MAP, scripts/mod_file_paths.py)
    # when one is in the environment, as run.sh exports it: here the game's own files are read.
    import mod_file_paths
    mod_file_paths.configure({})
    has_view = bool(view_dir) and Path(view_dir).resolve() != Path(game_dir).resolve()
    has_map = bool(file_map) and Path(file_map).is_file()
    has_plan = bool(archive_plan) and Path(archive_plan).is_file()
    if not (has_view or has_map or has_plan):
        return [], 0                            # no mod: the game's archives are not even opened
    game = GameIndex(game_dir)
    reasons, looked = [], 0
    # loose mods (scripts/mods.py): the view's files that are not the game's own
    if view_dir and Path(view_dir).resolve() != Path(game_dir).resolve():
        view = Path(view_dir)
        for path in sorted(view.rglob('*')):
            if not path.is_file():
                continue
            relative = path.relative_to(view).as_posix()
            original = Path(game_dir) / relative
            try:
                if original.is_file() and os.path.samefile(path, original):
                    continue
            except OSError:
                pass
            looked += 1
            if relative.lower().startswith('archives/') and relative.lower().endswith('.pak'):
                reasons += archive_reasons(path, game, f'mod archive {path.name}')
            elif original.is_file() and original.read_bytes() == path.read_bytes():
                continue
            else:
                reasons.append(f'a mod changes the game folder file {relative}')
    # mod packages (scripts/mod_content.py): whole files redirected, entries replaced in archives
    own = Path(own_dir).resolve() if own_dir else None
    for relative, source in (read_file_map(file_map) if file_map and Path(file_map).is_file() else []):
        if own and Path(source).resolve().is_relative_to(own):
            continue                            # made by the port itself
        looked += 1
        if relative.lower().startswith('archives/') and relative.lower().endswith('.pak'):
            reasons += archive_reasons(source, game, f'mod archive {Path(relative).name}')
        else:
            reasons += [f'mod file {relative}: {r}' for r in entry_reasons(relative, Path(source).read_bytes(), game)]
    if archive_plan and Path(archive_plan).is_file():
        from mod_components import payload_bytes
        for record in json.loads(Path(archive_plan).read_text(encoding='utf-8')):
            op, folder = record['operation'], Path(record['folder'])
            looked += 1
            kind = op.get('kind', '')
            if kind == 'localized_strings' or kind.startswith('shader_'):
                continue
            if kind != 'archive_asset':
                reasons.append(f'a mod package operation "{kind}"')
                continue
            archive = game.archives.get(str(op.get('archive', '')).lower())
            if archive is None or not 0 <= op.get('index', -1) < len(archive.files):
                reasons.append(f'a mod package entry of {op.get("archive")}')
                continue
            try:
                data = payload_bytes(op, folder, archive.extract(archive.files[op['index']]))
            except (ValueError, OSError) as error:
                reasons.append(f'a mod package entry {op.get("asset")} ({error})')
                continue
            reasons += [f'mod entry {op.get("asset")}: {r}' for r in entry_reasons(op['asset'], data, game)]
    return reasons, looked


def stamp(paths):
    """What the result depends on: these files' sizes and times (folders: their files')"""
    h = hashlib.sha256(Path(__file__).read_bytes())
    for p in paths:
        p = Path(p) if p else None
        if not p or not p.exists():
            h.update(b'-')
            continue
        files = [p] if p.is_file() else sorted(x for x in p.rglob('*') if x.is_file())
        for f in files:
            st = f.stat()
            h.update(f'{f}|{st.st_size}|{st.st_mtime_ns}'.encode())
    return h.hexdigest()


def classify(game_dir, view_dir, out, file_map=None, archive_plan=None, layout_dir=None, patch_dirs=(),
             app_version='01.21', title_id='CUSA13795'):
    """(class, reasons) of this launch; cached in out/online-content.json until its inputs change"""
    out = Path(out)
    applied_path = out / 'patches-applied.json'
    applied = json.loads(applied_path.read_text(encoding='utf-8')) if applied_path.is_file() else []
    key = stamp([view_dir, file_map, archive_plan, applied_path, layout_dir, *patch_dirs]) + app_version + title_id
    cache = out / 'online-content.json'
    try:
        kept = json.loads(cache.read_text(encoding='utf-8'))
        if kept.get('stamp') == key:
            return kept['class'], kept['reasons']
    except (OSError, ValueError, KeyError):
        pass
    reasons = patch_reasons(applied, patch_dirs, layout_dir, app_version, title_id)
    mods, looked = mod_reasons(game_dir, view_dir, file_map, archive_plan, out)
    reasons += mods
    result = MODIFIED if reasons else CHARACTERS if looked else CLEAN
    try:
        cache.write_text(json.dumps({'stamp': key, 'class': result, 'reasons': reasons}), encoding='utf-8')
    except OSError:
        pass
    return result, reasons


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--game', required=True)
    p.add_argument('--view')
    p.add_argument('--out', required=True)
    p.add_argument('--file-map')
    p.add_argument('--archive-plan')
    p.add_argument('--layout-dir')
    p.add_argument('--patches-dir', action='append', default=[])
    p.add_argument('--app-version', default='01.21')
    p.add_argument('--title-id', default='CUSA13795')
    a = p.parse_args()
    result, reasons = classify(a.game, a.view or a.game, a.out, a.file_map, a.archive_plan, a.layout_dir,
                               a.patches_dir, a.app_version, a.title_id)
    print(f'class={result}')
    for reason in reasons:
        print(reason)


if __name__ == '__main__':
    main()
