#!/usr/bin/env python3
"""Measure a fixed My Device double tap against an exact final-frame reference.

Requires dbt/emu-patch/add_classpad_benchmark.py in the emulator. Flash must
already be flattened with nkflash.py. Run comparisons serially for wall time.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('emulator', 'binary', 'flash', 'reference', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--flash-offset', default='0x03860000')
    p.add_argument('--tap-tick', type=int, default=7500000)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    inputs = {
        name: {'path': str(getattr(a, name).resolve()),
               'sha256': hashlib.sha256(getattr(a, name).read_bytes()).hexdigest()}
        for name in ('emulator', 'binary', 'flash', 'reference')
    }
    env = dict(os.environ, CALCEMU_MODEL='fx-cp400', SDL_VIDEODRIVER='dummy',
               SDL_RENDER_DRIVER='software', HLE_FBDUMP_ALL='1',
               HLE_FBDUMP=str((a.output / 'final.ppm').resolve()),
               HLE_FB_EXPECT=str(a.reference.resolve()),
               HLE_OSROM=str(a.flash.resolve()), HLE_OSROM_PAD=a.flash_offset,
               HLE_CB_TOUCHSEQ=f'35,40,{a.tap_tick},4000;35,40,{a.tap_tick+8000},4000',
               HLE_EXIT_AT='30000000')
    start = time.monotonic()
    events = []
    with subprocess.Popen([str(a.emulator.resolve()), str(a.binary.resolve())],
                          env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True) as proc:
        with (a.output / 'run.log').open('w') as log:
            for line in proc.stdout:
                log.write(line)
                log.flush()
                if 'INJECTTOUCHSEQ' in line or '[BENCH loaded]' in line:
                    event = dict(wall_seconds=time.monotonic()-start, message=line.strip())
                    events.append(event)
                    print(event, flush=True)
        proc.wait()
    result = dict(inputs=inputs, tap_tick=a.tap_tick, flash_offset=a.flash_offset,
                  host_sample_start=os.environ.get('HLE_DBT_SAMPLE'),
                  host_sample_layout={k: os.environ[k] for k in (
                      'HLE_DBT_SAMPLE_BLOCKS', 'HLE_DBT_SAMPLE_COUNTS',
                      'HLE_DBT_SAMPLE_PER_BANK', 'HLE_DBT_COUNTER_BASE',
                      'HLE_DBT_COUNTER_WORDS') if k in os.environ},
                  events=events, returncode=proc.returncode,
                  total_wall_seconds=time.monotonic()-start)
    loaded = next((e for e in events if '[BENCH loaded]' in e['message']), None)
    release = next((e for e in events if f'up] at tick {a.tap_tick+12000}' in e['message']), None)
    if loaded and release:
        tick = int(re.search(r'tick=(\d+)', loaded['message'])[1])
        result['load_ticks'] = tick-a.tap_tick-12000
        result['load_model_seconds'] = result['load_ticks']/32768
        result['load_wall_seconds'] = loaded['wall_seconds']-release['wall_seconds']
    result['matched'] = bool(loaded and release and proc.returncode == 0)
    (a.output / 'timing.json').write_text(json.dumps(result, indent=2)+'\n')
    return 0 if result['matched'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
