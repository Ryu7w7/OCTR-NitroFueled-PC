#!/usr/bin/env python3
"""Read an existing Linux CTR process without rebuilding, pausing, or writing to it.

Usage: /usr/bin/python3 tools/re/presence_live.py PID [--samples 3]
Requires permission to read /proc/PID/mem. Reports JSON, never publishes to Discord.
This diagnostic validates the game layout, not the compiled runtime writer.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import sys
import time


class Game:
    def __init__(self, pid):
        self.fd = os.open(f'/proc/{pid}/mem', os.O_RDONLY)
        self.base = None
        for line in Path(f'/proc/{pid}/maps').read_text().splitlines():
            span, perms, *_ = line.split()
            if 'r' not in perms or 'x' not in perms:
                continue
            start = int(span.split('-')[0], 16)
            for base in (start, start - 0x4000):
                if self.read(base + 0x94efa0, 7) == bytes.fromhex('488b05314d1602'):
                    self.base = base
                    return
        os.close(self.fd)
        raise ValueError('EU 1.21 getter signature not found')

    def read(self, address, size):
        if address < 4096 or address > (1 << 63) - size:
            return b''
        try:
            return os.pread(self.fd, size, address)
        except OSError:
            return b''

    def value(self, address, fmt='Q', default=0):
        data = self.read(address, struct.calcsize('<' + fmt))
        return struct.unpack('<' + fmt, data)[0] if len(data) == struct.calcsize('<' + fmt) else default

    def items(self, address, limit):
        count = self.value(address + 0xc, 'i', -1)
        if not 0 <= count <= limit:
            raise ValueError('Invalid or changing guest list')
        data = self.value(address + 0x20)
        return [self.value(data + 8*i) for i in range(count)]

    def snapshot(self):
        q, b = self.value, self.base
        manager = q(b + 0x2ab3cd8)
        mode = {1: 'Adventure', 8: 'Local Arcade', 16: 'Online'}.get(q(manager + 0x144, 'i'), '')
        loading = q(manager + 0x88)
        if q(loading) != b + 0x2540eb8:
            return {'phase': 'Starting game'}
        loading_state = q(loading + 0xc, 'i', -1)
        if loading_state in (1, 2, 3, 4):
            return {'phase': 'Loading'}
        if loading_state != 0:
            return {}
        track = self.read(q(manager + 0xc0), 160).split(b'\0')[0]
        # Racers survive a return to the frontend; the loaded level takes precedence.
        if track.rsplit(b'/', 1)[-1].lower() in (b'octane_startscreen', b'octane_onlinelobby',
                                                b'octane_scrapbook', b'octane_credits'):
            return {'phase': 'In menus'}
        result = {'phase': 'In menus'}
        racer_list = q(q(b + 0x2ad4b70) + 0x10)
        racer = next((p for p in (self.items(racer_list, 16) if racer_list else [])
                      if q(p) == b + 0x26d5f78 and q(p + 0x40, 'B', 1) == 0
                      and q(p + 0xb0, 'B') == 1), None)
        if racer is None:
            return result
        races = ('', 'Single Race', 'Time Trial', 'Relic Race', 'Boss Race', 'CTR Challenge',
                 'Cup Race', 'Adventure Hub', 'Crystal Challenge', 'Limit Battle',
                 'Capture the Flag', 'Last Kart Driving', 'Steal the Bacon', 'Crystal Grab',
                 'Sports', 'Ring Rally')
        race = q(q(b + 0x2acde48) + 0xd4c, 'i')
        if not 0 < race < len(races):
            raise ValueError('Invalid or changing race mode')
        finished = q(racer + 0x8c, 'B')
        result.update(mode=mode, race=races[race], phase='Exploring' if race == 7 else 'Finished' if finished else 'Racing')
        lap, laps = q(racer + 0x4c, 'i', -1), q(q(b + 0x2ad4e30) + 0xc, 'i')
        if 1 <= race <= 6 and 0 <= lap <= laps+1 and 1 <= laps <= 99:
            result.update(lap=max(1, min(lap, laps)), laps=laps)
        if track and all(32 <= c < 127 for c in track):
            result['track'] = track.decode('ascii')
        handle, pool = q(b + 0x2a92888, 'I'), q(b + 0x2a92880)
        index, generations = handle & 0x7fff, q(pool + 0x28)
        if not handle & 0x8000 or handle == q(b + 0x2a922d0, 'I'):
            return result
        if index >= q(generations + 0xc, 'i') or q(q(generations + 0x20) + 2*index, 'H') != index | 0x8000:
            return result
        entity = q(q(pool + 0x20) + 8*index)
        clock = next((p for p in self.items(q(entity + 0x18), 256)
                      if q(p) == b + 0x2592f98 and not q(p + 0x20, 'B', 0x10) & 0x10), None)
        if clock is None:
            return result
        timer = q(clock + 0x28)
        clock_mode = q(q(clock + 0x48) + 0xc, 'i', -1)
        seconds = -1
        if clock_mode == 1:
            seconds = q(clock + 0x3c, 'f', -1)
        elif clock_mode == 0 and q(timer + 0x18, 'i', -1) == 0:
            start, end = q(timer + 0xc, 'i', -1), q(timer + 0x10, 'i', -1)
            running, stopped = q(timer + 0x14, 'B'), q(timer + 0x15, 'B')
            if running and not stopped:
                end = q(q(b + 0x2ab4100) + 0x20, 'i', -1)
            if start >= 0 and end >= start:
                seconds = (end - start) / 8192
            if not running and not finished:
                result['phase'] = 'Starting race'
        if not finished and q(q(b + 0x2ab4100) + 0x28, 'B') == 1:
            result['phase'] = 'Paused'
        if not finished and race != 7 and 0 <= seconds < 86400:
            result.update(race_seconds=int(seconds), precise_seconds=seconds)
            if result['phase'] == 'Racing' and q(timer + 0x14, 'B') and not q(timer + 0x15, 'B'):
                key = 'timer_end' if clock_mode == 1 else 'timer_start'
                result[key] = round(time.time() + seconds if clock_mode == 1 else time.time() - seconds)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('--samples', type=int, default=1)
    args = parser.parse_args()
    if not 1 <= args.samples <= 60:
        parser.error('--samples must be between 1 and 60')
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'launcher'))
    from sacredfire_launcher import game_presence
    game = Game(args.pid)
    try:
        for i in range(args.samples):
            state = game.snapshot()
            details, status = game_presence(state)
            print(json.dumps({'at': time.time(), 'pid': args.pid, 'image': hex(game.base),
                              'game': state, 'discord_details': details, 'discord_state': status}), flush=True)
            if i + 1 < args.samples:
                time.sleep(2)
    finally:
        os.close(game.fd)


if __name__ == '__main__':
    main()
