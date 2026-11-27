#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's Tools > Content page (launcher.cpp ContentPage): the game's new tracks
(scripts/track_content.py) scanned against the server's list, or downloaded again.

  content_cli.py scan   --settings bbport.ini [--content DIR] [--progress FILE]
  content_cli.py repair --settings bbport.ini [--content DIR] [--progress FILE]

One JSON object is printed last:
  scan:   {"server": bool, "tracks": [{"id", "name", "version", "state", "bad"}], "problems": n}
          state: ok, missing, outdated or damaged
  repair: {"ok": bool, "changed": [ids], "error": "..."}
  either: {"error": "..."} when the list of the tracks could not be had
--progress FILE: the percent done, rewritten as it goes ("scan 40" / "tracks 40 <name>"), for the bar.
"""
import argparse
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for p in (ROOT, ROOT / 'scripts'):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

import track_content  # noqa: E402


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=('scan', 'repair'))
    p.add_argument('--settings', type=Path)
    p.add_argument('--content', type=Path)
    p.add_argument('--progress', type=Path)
    a = p.parse_args()

    def log(line):
        if line.startswith('@progress '):
            if a.progress:
                try:
                    tmp = a.progress.with_suffix('.tmp')
                    tmp.write_text(line[len('@progress '):], encoding='utf-8')
                    os.replace(tmp, a.progress)
                except OSError:
                    pass
        else:
            print(line, flush=True)

    try:
        if a.action == 'scan':
            out = track_content.verify(a.settings, a.content, log=log)
        else:
            out = {'ok': True, 'changed': track_content.repair(a.settings, a.content, log=log)}
    except track_content.RequiredContent as error:
        out = {'ok': False, 'error': str(error)} if a.action == 'repair' else {'error': str(error)}
    except (OSError, ValueError, KeyError) as error:
        out = {'ok': False, 'error': str(error)}
    finally:
        if a.progress:
            try:
                a.progress.unlink()
            except OSError:
                pass
    print(json.dumps(out, separators=(',', ':')), flush=True)   # compact: the launcher's JsonField


if __name__ == '__main__':
    main()
