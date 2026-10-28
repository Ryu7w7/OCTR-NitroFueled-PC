#!/usr/bin/env python3
"""No-build presence bridge for existing Linux AppImages; installed beside presence_live.py.

Run as: python3 tools/presence_bridge.py bin/sacredfire GAME_ARGUMENTS...
The reader is the game's parent so Linux's parent-only ptrace policy works too.
New source builds use runtime_state.c instead; do not install both writers.
"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from presence_live import Game


def main():
    if len(sys.argv) < 2:
        raise SystemExit('Expected the existing game executable and its arguments')
    process = subprocess.Popen(sys.argv[1:])
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda signum, _frame: process.send_signal(signum) if process.poll() is None else None)
    config = Path(os.environ.get('BB_CONFIG', str(Path(os.environ.get('BB_DATA_DIR', '.')) / 'bbport.ini')))
    state = config.parent / 'ryunet_state.txt'
    temporary = state.with_name(f'.ryunet_state.{process.pid}.tmp')
    game = None
    reported = False
    next_sample = 0
    try:
        while process.poll() is None:
            if time.monotonic() >= next_sample:
                next_sample = time.monotonic() + 1
                values = {}
                try:
                    if game is None:
                        game = Game(process.pid)
                    values = game.snapshot()
                    if not reported:
                        print('Discord game state: no-build AppImage bridge connected', flush=True)
                        reported = True
                except (OSError, ValueError):
                    pass  # image/objects may not exist yet, or are being unloaded
                values = {k: v for k, v in values.items()
                          if k in ('phase', 'mode', 'race', 'track', 'lap', 'laps', 'race_seconds', 'timer_start', 'timer_end')}
                values.update(at=int(time.time()), pid=process.pid)
                try:
                    temporary.write_text(''.join(f'{k}={v}\n' for k, v in values.items()), encoding='utf-8')
                    os.replace(temporary, state)
                except OSError as error:
                    print(f'Discord game state: cannot write snapshot: {error}', flush=True)
                    next_sample = time.monotonic() + 30
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                continue
        return process.returncode if process.returncode >= 0 else 128 - process.returncode
    finally:
        if game is not None:
            os.close(game.fd)
        try:
            if f'pid={process.pid}\n' in state.read_text():
                state.unlink()
        except OSError:
            pass
        temporary.unlink(missing_ok=True)


if __name__ == '__main__':
    sys.exit(main())
