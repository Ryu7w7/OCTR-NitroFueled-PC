# SPDX-License-Identifier: GPL-2.0-or-later
"""Atomic, per-session status messages for the native Crash NL sync overlay.

The coordinator must refresh active messages at least once per second. This is
display state only: it neither installs files nor releases a matchmaking hold.
Pass this absolute file path as BB_MOD_SYNC_STATUS to an overlay-enabled runtime.
"""
import math
import os
from pathlib import Path
import tempfile
import time

PHASES = {'checking', 'downloading', 'verifying', 'ready', 'error', 'fallback', 'hidden'}


def publish(path, phase, label='', completed=0, total=0, *, now=None):
    if phase not in PHASES:
        raise ValueError('Unknown sync phase')
    if (type(completed) is not int or type(total) is not int or
            not 0 <= completed <= total <= 2**53):
        raise ValueError('Progress must be integer counts: 0 <= completed <= total <= 2**53')
    stamp = time.time() if now is None else now
    if not math.isfinite(stamp) or stamp < 0:
        raise ValueError('Invalid timestamp')
    # One bounded UTF-8 line. Never let a mod title inject protocol fields.
    label = ''.join(c if c >= ' ' and c != '\x7f' else ' ' for c in str(label))[:160]
    payload = f'CRASHNL_SYNC_1\n{stamp:.3f}\n{phase}\n{completed}\n{total}\n{label}\n'
    path = Path(path).resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + '.', suffix='.tmp', dir=path.parent)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as stream:
            stream.write(payload)
        # Reader opens with delete sharing on Windows, so replacement is safe.
        # Windows scanners and screenshot/overlay software can briefly hold a
        # handle without delete sharing. Preserve the last complete status and
        # retry rather than aborting the asset transfer on the first collision.
        for attempt in range(21):
            try:
                os.replace(temporary, path)
                break
            except PermissionError:
                if attempt == 20:
                    raise
                time.sleep(0.1)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
