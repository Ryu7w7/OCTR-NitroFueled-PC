"""Background orchestration; the game thread never waits for a download.

SPDX-License-Identifier: GPL-2.0-or-later
"""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import threading
import time
import uuid

import mod_library
from mod_components import read_ini, save_json, sha, under
from .cache import Cache
from .models import MAX_OFFERS
from . import status
from .transport import Cancelled


def enabled_offers(mods, selection, api, log):
    """Only explicitly enabled, unchanged original ZIPs with creator metadata.

    Folder/loose-PAK mods still play normally; there is no authoritative outer
    download identity for them, so the optional network feature skips them.
    """
    if selection is None:
        return []
    library = mod_library.read(mods).get('entries', {})
    selected = json.loads(selection.read_text()) if selection.exists() else {}
    enabled = selected.get('enabled', [])
    if not isinstance(enabled, list):
        raise ValueError('Invalid mod selection')
    offers = []
    for ident, item in sorted(library.items()):
        if mod_library.PREFIX + ident not in enabled or len(offers) == MAX_OFFERS:
            continue
        try:
            source = under(mods, item['source'])
            if source.suffix.lower() != '.zip' or not source.is_file() or source.is_symlink():
                continue
            if sha(source) != item['sha256']:
                continue
            offers.append(api.pin(source))
        except (OSError, ValueError, KeyError) as error:
            log('Mod sync: skipped one local package (' + type(error).__name__ + ')')
    return offers


class Service:
    """Lifetime owned by the runner. A loaded proof MUST come from a runtime adapter.

    Default loaded_provider=None never claims readiness. Cache/import are distinct
    from mounted, registered and usable assets. Current runner uses restart fallback.
    """
    def __init__(self, config, mods, selection, client, *, cache=None, log=print,
                 loaded_provider=None, poll_seconds=2):
        self.config = Path(config)
        self.mods = Path(mods)
        self.selection = Path(selection) if selection is not None else None
        self.client = client
        self.state = self.mods.parent / 'mod-state' / 'sync'
        self.cache = cache or Cache(self.state / 'downloads')
        self.log = log
        self.loaded_provider = loaded_provider
        self.poll_seconds = poll_seconds
        self.cancelled = threading.Event()
        self.thread = None
        self.status_path = self.state / 'sessions' / (uuid.uuid4().hex + '.txt')
        self.progress_lock = threading.Lock()
        self.downloaded_bytes = 0
        self.cache.progress = self.on_progress

    def on_progress(self, completed, total):
        with self.progress_lock:
            self.downloaded_bytes = max(0, min(completed, total))

    def enabled(self):
        try:
            return read_ini(self.config).get('mod_sync') == '1'
        except OSError:
            return False

    def start(self):
        if not self.enabled():
            return self
        self.thread = threading.Thread(target=self._run, name='octr-mod-sync', daemon=True)
        self.thread.start()
        return self

    def close(self):
        self.cancelled.set()
        try:
            self.publish('hidden', '')
        except OSError:
            pass
        if self.thread:
            self.thread.join(timeout=0.2)  # never keep the game window open for an HTTP timeout

    def publish(self, phase, label, completed=0, total=0):
        status.publish(self.status_path, phase, label, completed, total)

    def _run(self):
        try:
            self._loop()
        except Cancelled:
            pass
        except Exception as error:
            # Do not print HTTP bodies, auth headers, nicknames, or server tokens.
            self.log('Mod sync unavailable (' + type(error).__name__ + '); normal fallback remains active.')
            try:
                self.publish('fallback', 'Sync unavailable; race continues with normal fallback')
            except OSError:
                pass

    def _loop(self):
        if not self.client.capabilities():
            self.publish('fallback', 'This server does not support optional mod sync yet')
            return
        offers = enabled_offers(self.mods, self.selection, self.cache.api, self.log)
        report = {'phase': 'preparing', 'session': '', 'generation': '', 'loaded': []}
        failed = set()
        cached = set()
        future = current = None
        last_ui = None
        # Single transfer at a time; coordinator polling keeps heartbeat/UI alive independently.
        with ThreadPoolExecutor(max_workers=1, thread_name_prefix='octr-mod-download') as pool:
            try:
                while not self.cancelled.is_set() and self.enabled():
                    snapshot = self.client.exchange(offers, report)
                    if snapshot is None:
                        report = {'phase': 'preparing', 'session': '', 'generation': '', 'loaded': []}
                        self.publish('hidden', '')
                        last_ui = None
                        self.cancelled.wait(self.poll_seconds)
                        continue
                    if future and future.done():
                        try:
                            future.result()
                            cached.add(current.sha256)
                        except Cancelled:
                            raise
                        except Exception as error:
                            failed.add(current.sha256)
                            self.log('Mod sync download rejected (' + type(error).__name__ + '); using fallback.')
                        future = current = None
                    required = {o.sha256 for o in snapshot.offers}
                    # Native adapter proves precisely this session/generation; no disk-only ack.
                    loaded = (set(self.loaded_provider(snapshot)) if self.loaded_provider else set()) & required
                    for offer in snapshot.offers:
                        if offer.sha256 in loaded | failed | cached or future:
                            continue
                        current = offer
                        self.on_progress(0, offer.bytes)
                        future = pool.submit(self.cache.stage, offer, self.cancelled)
                    if required <= loaded:
                        phase, reason = 'ready', 'Required mods are loaded' if required else 'No shared mods required'
                    elif future:
                        phase, reason = 'preparing', 'Downloading GameBanana mods; race wait is limited to 20 seconds'
                    else:
                        phase, reason = 'fallback', 'Downloaded mods need review and restart; using normal fallback'
                    report = {'phase': phase, 'session': snapshot.session, 'generation': snapshot.generation,
                              'loaded': sorted(loaded)}
                    save_json(self.state / 'status.json', {'phase': phase, 'reason': reason,
                        'session': snapshot.session, 'generation': snapshot.generation,
                        'cached': sorted(cached), 'failed': sorted(failed), 'hold': snapshot.hold})
                    ui_phase = 'downloading' if phase == 'preparing' else phase
                    if not self.cancelled.is_set():
                        total = sum(o.bytes for o in snapshot.offers)
                        done = sum(o.bytes for o in snapshot.offers if o.sha256 in loaded | cached)
                        if current and current.sha256 in required - loaded - cached:
                            with self.progress_lock:
                                done += self.downloaded_bytes
                        ui = (ui_phase if required else 'hidden', reason, min(done, total), total)
                        if phase == 'preparing' or ui != last_ui:
                            self.publish(*ui)
                            last_ui = ui
                    if self.cancelled.wait(self.poll_seconds):
                        break
            finally:
                # Immediate best-effort withdrawal; server deadline/heartbeat is the final guard.
                self.cancelled.set()
                report['phase'] = 'fallback'
                try:
                    self.client.exchange([], report)
                except Exception:
                    pass


def start_for_runner(config, data, env, proof, log):
    """Windows run.py integration; no traffic or status files when the toggle is off."""
    try:
        env.pop('BB_MOD_SYNC_STATUS', None)
        if read_ini(Path(config)).get('mod_sync') != '1' or env.get('BB_ONLINE') != '1':
            return None
        if not env.get('BB_ONLINE_KEYHASH'):
            return None
        from .protocol import RyuNet
        host = env['BB_ONLINE_SERVER']
        if any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-' for c in host):
            raise ValueError('Use a DNS server name for optional HTTPS mod sync')
        client = RyuNet('https://' + host, env['BB_ONLINE_NAME'], proof,
                        clock_offset=float(env.get('BB_ONLINE_CLOCK_OFFSET', '0')))
        mods = Path(env.get('BB_MODS_DIR', Path(data) / 'mods'))
        selection = Path(env.get('BB_PATCHES_CONFIG', Path(data) / 'patches.json'))
        if env.get('BB_MODS_ENABLED') == '0' and not env.get('BB_MOD_ARCHIVE_PLAN'):
            selection = None
        service = Service(config, mods, selection, client, log=log)
        env['BB_MOD_SYNC_STATUS'] = str(service.status_path.resolve())
        return service.start()
    except Exception as error:
        log('Optional mod sync could not start (' + type(error).__name__ + '); continuing normally.')
        return None
