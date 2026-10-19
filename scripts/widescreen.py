"""Built-in verified 21:9/32:9 layouts. Author: tcosi. GPL-2.0-or-later."""
from pathlib import Path
import atexit, hashlib, json, os, shutil, subprocess, sys, tempfile, uuid
import xml.etree.ElementTree as ET
from nitro_components import (apply_shader, copy_tree, patch_archive, read_ini,
                              save_json, sha, snapshot, under)
import nitro_format as fmt
from mod_session import launch_lock, sync_saves

DATA = Path(__file__).resolve().parent / 'widescreen_data'
OUTPUTS = {'21x9': ('3440x1440', '2560x1080'),
           '32x9': ('5120x1440', '3840x1080', '2560x720', '7680x2160')}
ACTIVE = None
FROZEN_RENDER = None


def selection(settings):
    """The existing upstream switch also selects the matching corrected layout."""
    if settings.get('ultrawide') != '1':
        return None
    size = settings.get('ultrawide_size', settings.get('output_res', ''))
    layout = settings.get('widescreen_layout', '')
    if not layout:
        layout = next((k for k, sizes in OUTPUTS.items() if size in sizes), '')
    if layout not in OUTPUTS or size not in OUTPUTS[layout]:
        raise ValueError('Select a supported 21:9 or 32:9 output in Display settings.')
    internal = settings.get('render_res', '1080')
    if internal not in ('480', '720', '1080', '1440', '1620', '2160'):
        raise ValueError('Unsupported widescreen internal resolution: ' + internal)
    return layout, dict(settings, output_res=size, render_res=internal)


def preflight(game, patchdir, config, selected):
    from prepare import parse_self
    from patches import (external_patches, external_selection, render_patches,
                         compile_external, eboot_segments)
    elf, *_ = parse_self((game / 'eboot.bin').read_bytes())
    patches = external_selection(external_patches(patchdir), selected, read_ini(config))
    patches += render_patches(read_ini(config))
    claims = []
    for key, path, meta in patches:
        writes = compile_external([(key, path, meta)], eboot_segments(elf), elf)
        if len(writes) != len(list(meta.iter('Line'))):
            raise ValueError('Incomplete executable patch: ' + key)
        for offset, value in writes:
            if any(offset < old + len(raw) and old < offset + len(value)
                   for old, raw, owner in claims):
                raise ValueError('Overlapping executable patch: ' + key)
            claims.append((offset, value, key))
    return len(claims)


def ensure_shaders(port, state, source, user, operations, environment, settings):
    from nitro_shader_setup import requirements, wait_for_setup
    missing, wrong = requirements(user, operations)
    if wrong:
        raise ValueError('Incompatible v0.5.3 shader cache: ' + ', '.join(wrong))
    if not missing:
        return
    setup = state / 'shader-setup' / uuid.uuid4().hex
    setup.mkdir(parents=True)
    baseline = dict(settings, ultrawide='0', widescreen_layout='', output_res='1920x1080',
                    render_res='1080', patch_pro_1080p='0', temporal='off', upscaler='off', online='0')
    ini = setup / 'bbport.ini'
    ini.write_text(''.join(k + '=' + v + '\n' for k, v in baseline.items()), encoding='utf-8')
    save_json(setup / 'patches.json', {'enabled': [], 'disabled': []})
    setup_user = setup / 'user'
    setup_user.mkdir()
    # Existing profile metadata from another engine must not enter this cache.
    savedata = state / 'user/savedata'
    if savedata.exists():
        copy_tree(savedata, setup_user / 'savedata')
    env = {k: v for k, v in environment.items() if not k.startswith('BB_')}
    env.update(BB_WIDESCREEN_SETUP='1', BB_GAME_DIR=str(source), BB_CONFIG=str(ini),
               BB_DATA_DIR=str(setup), BB_USER_DIR=str(setup_user), BB_ONLINE='0',
               BB_MODS_ENABLED='0', BB_PATCHES_DIR=str(port / 'patches'),
               BB_PATCHES_CONFIG=str(setup / 'patches.json'))
    # Reuse only the public import-name inputs, never another installation's data.
    out = setup / 'out'
    out.mkdir()
    for name in ('aerolib.inl', 'import_names.txt'):
        p = Path(environment.get('BB_DATA_DIR', port)) / 'out' / name
        if p.is_file():
            shutil.copy2(p, out / name)
    python = port / 'python/python.exe'
    print('Widescreen: preparing original v0.5.3 shaders. Reach the main menu if asked; '
          'the setup game will close normally and restart with the corrected layout.', flush=True)
    with (setup / 'console.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen([str(python) if python.exists() else sys.executable,
                                    '-X', 'utf8', str(port / 'run.py')], cwd=port,
                                   env=env, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        wait_for_setup(process, setup_user, operations, port / 'bin/sacredfire.exe',
                       lambda stage, message: print(message, flush=True))
    save_json(setup / 'result.json', {'exit_code': 0, 'required_shaders_verified': True})
    copy_tree(setup_user / 'cache', state / 'vanilla-cache' / sha(port / 'bin/sacredfire.exe'))
    copy_tree(setup_user / 'cache', user / 'cache')
    if (setup_user / 'savedata').exists():
        copy_tree(setup_user / 'savedata', user / 'savedata')


def finish(code):
    if ACTIVE is None:
        return
    session, state = ACTIVE
    save_json(session / 'result.json', {'exit_code': code})
    if code == 0:
        sync_saves(session / 'user/savedata', state / 'user/savedata', state / 'save-backups')


def validate_restart(config):
    if FROZEN_RENDER is not None:
        now = read_ini(config)
        if any(now.get(k) != v for k, v in FROZEN_RENDER.items()):
            raise ValueError('Widescreen rendering changed in-game. Close the game and select '
                             'the output/internal resolution in the launcher before Play.')


def prepare(port, data, environment, game, config, prepare_only=False):
    global ACTIVE, FROZEN_RENDER
    port, data, game, config = map(Path, (port, data, game, config))
    base = read_ini(config)
    chosen = selection(base)
    if chosen is None or environment.get('BB_WIDESCREEN_SETUP') == '1':
        return game, str(config)
    layout, selected_settings = chosen
    folder = DATA / ('ultrawide-' + layout)
    manifest = fmt.validate_folder(folder)
    variant, flat = fmt.resolve(manifest, selected_settings)
    compat = manifest['compatibility']
    engine = sha(port / 'bin/sacredfire.exe')
    if engine not in compat['runtime_sha256'] or (port / 'bin/VERSION').read_text().strip() not in compat['runtime_versions']:
        raise ValueError('The built-in widescreen fixes need their matching v0.5.3 engine.')
    if sha(game / 'eboot.bin') not in compat['eboot_sha256']:
        raise ValueError('These verified widescreen assets require the tested USA 1.21 executable.')
    if environment.get('BB_ONLINE', base.get('online', '0')) == '1':
        raise ValueError('This widescreen test build is offline-only. Turn Online off before Play.')
    import mods
    if environment.get('BB_MODS_ENABLED', '1') != '0' and mods.selected(
            Path(environment.get('BB_MODS_DIR', data / 'mods')),
            Path(environment.get('BB_MODS_CONFIG', data / 'mods.json'))):
        raise ValueError('Disable loose-file mods for this built-in widescreen test.')
    state = data / 'widescreen'
    lock = launch_lock(state)
    lock.__enter__()
    try:
        source_state = snapshot(game)
        # Internal resolution changes reuse the same verified archive set.
        identity = {'source': str(game.resolve()), 'files': source_state,
                    'manifest': sha(folder / 'nitro-mod.json'), 'layout': layout,
                    'asset_set': next(v for v in manifest['variants'] if v['id'] == variant)['useOperations']}
        key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:24]
        profile = state / 'profiles' / key
        ready = profile / 'ready.json'
        if ready.exists():
            record = json.loads(ready.read_text())
            if record['identity'] != identity or record['files'] != snapshot(profile / 'game'):
                raise ValueError('Prepared widescreen files changed; retained for inspection: ' + str(profile))
        else:
            profile.parent.mkdir(parents=True, exist_ok=True)
            if shutil.disk_usage(profile.parent).free < sum(v[0] for v in source_state.values()) + 1024**3:
                raise ValueError('Insufficient space for a private widescreen game copy.')
            stage = Path(tempfile.mkdtemp(prefix='building-', dir=profile.parent))
            print('Widescreen: preparing verified ' + layout + ' menu and HUD assets…', flush=True)
            copy_tree(game, stage / 'game')
            groups = {}
            for op in flat['operations']:
                if op['kind'] == 'archive_asset':
                    groups.setdefault(op['archive'], []).append((op, folder))
            for archive, operations in groups.items():
                patch_archive(stage / 'game/archives' / archive, operations)
            if snapshot(game) != source_state:
                raise ValueError('Original game changed during preparation.')
            save_json(stage / 'ready.json', {'identity': identity, 'files': snapshot(stage / 'game')})
            os.replace(stage, profile)
        session = state / 'sessions' / uuid.uuid4().hex
        session.mkdir(parents=True)
        patchdir = session / 'patches'
        patchdir.mkdir()
        source_patches = Path(environment.get('BB_PATCHES_DIR', port / 'patches'))
        for path in source_patches.glob('*.xml'):
            shutil.copy2(path, patchdir / path.name)
        xml = ET.Element('Patch')
        ET.SubElement(ET.SubElement(xml, 'TitleID'), 'ID').text = 'CUSA13795'
        for rel in flat['patches']:
            for meta in ET.parse(under(folder, rel)).getroot().iter('Metadata'):
                meta.set('isEnabled', 'true')
                xml.append(meta)
        generated = patchdir / 'Built-In-Widescreen.xml'
        if generated.exists():
            raise ValueError('Reserved widescreen patch filename already exists.')
        ET.ElementTree(xml).write(generated, encoding='utf-8', xml_declaration=True)
        # A single owner for projection, scissor and HUD math: suppress upstream's
        # resolution/front-end/cinematic hooks while using the complete verified set.
        effective = dict(selected_settings, **flat.get('settings', {}))
        effective.update(ultrawide='0', ultrawide_frontend='0', ultrawide_cinematics='0')
        ini = session / 'bbport.ini'
        ini.write_text(''.join(k + '=' + v + '\n' for k, v in effective.items()), encoding='utf-8')
        selected = Path(environment.get('BB_PATCHES_CONFIG', data / 'patches.json'))
        selection_file = session / 'patches.json'
        save_json(selection_file, json.loads(selected.read_text()) if selected.exists() else {'enabled': [], 'disabled': []})
        writes = preflight(profile / 'game', patchdir, ini, selection_file)
        user = session / 'user'
        user.mkdir()
        if (state / 'user/savedata').exists():
            copy_tree(state / 'user/savedata', user / 'savedata')
        else:
            original = Path(environment.get('BB_USER_DIR', data / 'user')) / 'savedata'
            if original.exists():
                copy_tree(original, user / 'savedata')
        cache = state / 'vanilla-cache' / engine
        if cache.exists() and environment.get('BB_CLEAR_SHADER_CACHE') != '1':
            copy_tree(cache, user / 'cache')
        operations = [(op, folder) for op in flat['operations'] if op['kind'].startswith('shader_')]
        if not prepare_only:
            ensure_shaders(port, state, game, user, operations, environment, base)
            for op, package in operations:
                apply_shader(under(user, op['target']), op, package)
        environment.update(flat.get('environment', {}))
        environment.update(BB_CONFIG=str(ini), BB_USER_DIR=str(user), BB_PATCHES_DIR=str(patchdir),
                           BB_PATCHES_CONFIG=str(selection_file), BB_MODS_ENABLED='0', BB_CLEAR_SHADER_CACHE='0')
        for name in ('BB_FRONTEND_FOV', 'BB_CINEMATIC_16X9', 'BB_CUTSCENE'):
            environment.pop(name, None)
        save_json(session / 'launch.json', {'layout': layout, 'variant': variant, 'writes': writes,
                  'resolution': selected_settings['output_res'], 'engine': engine,
                  'prepare_only': prepare_only, 'source_unchanged': snapshot(game) == source_state})
        print('Widescreen: ' + layout + ', ' + selected_settings['output_res'] +
              ', verified menu/HUD fixes; ' + str(writes) + ' non-overlapping writes.', flush=True)
        ACTIVE = session, state
        FROZEN_RENDER = {k: effective.get(k) for k in ('output_res', 'render_res', 'temporal',
                         'upscaler', 'patch_pro_1080p', 'ultrawide')}
        atexit.register(lock.__exit__, None, None, None)
        return profile / 'game', str(ini)
    except BaseException:
        lock.__exit__(*sys.exc_info())
        raise
