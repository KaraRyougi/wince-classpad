#!/usr/bin/env python3
"""Exercise CE rotation via the emulated IIC touch sensor and verify its framebuffer.

Requires the diagnostic NK which autoruns classpad_rotate.exe, the ClassPad
emulator touch injector and atomic PPM dumps. No synthetic CE mouse messages.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('emulator', 'binary', 'flash', 'output'):
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--soft-key', action='store_true', help='Test four fixed-strip Rotate taps via background service')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    frame = args.output.resolve() / 'final.ppm'
    points = [(40, 40), (40, 487), (279, 487), (279, 40)]
    angles = [0, 90, 180, 270]
    if args.soft_key:
        points = [(251, 550)] * 4
        angles = [0, 90, 0, 90]
    seq = ';'.join(f'{x},{y},{7500000+i*400000},4000' for i, (x, y) in enumerate(points))
    env = dict(os.environ, CALCEMU_MODEL='fx-cp400', SDL_VIDEODRIVER='dummy',
               SDL_RENDER_DRIVER='software', HLE_FBDUMP_ALL='1', HLE_FBDUMP=str(frame),
               HLE_OSROM=str(args.flash.resolve()), HLE_OSROM_PAD='0x00EE0000',
               HLE_CB_TOUCHSEQ=seq, HLE_EXIT_AT='9000000')
    env.pop('HLE_FB_EXPECT', None)
    captured = []
    with subprocess.Popen([str(args.emulator.resolve()), str(args.binary.resolve())],
                          env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True) as process, (args.output / 'run.log').open('w') as log:
        for line in process.stdout:
            log.write(line); log.flush()
            if 'ROTATION ' in line or 'INJECTTOUCHSEQ' in line or 'ROTATEKEY:' in line or 'Rotate released' in line:
                print(line.rstrip(), flush=True)
            match = re.search(r'INJECTTOUCHSEQ down.*\((\d+)/4\)', line)
            if match:
                index = int(match[1]) - 1
                data = frame.read_bytes()
                (args.output / f'rotation-{angles[index]}-step-{index}.ppm').write_bytes(data)
                kind, dimensions, maxval, pixels = data.split(b'\n', 3)
                assert (kind, dimensions, maxval) == (b'P6', b'320 528', b'255')
                x, y = (40, 40) if args.soft_key else points[index]
                rgb = list(pixels[(y*320+x)*3:(y*320+x)*3+3])
                captured.append(dict(angle=angles[index], point=[x, y], rgb=rgb,
                                     blue=len(rgb) == 3 and rgb[2] > 200 and max(rgb[:2]) < 32))
        code = process.wait()
    log = (args.output / 'run.log').read_text()
    touches = re.findall(r'ROTATION touch phase=(\d+) x=(\d+) y=(\d+) PASS', log)
    passed = (code == 0 and 'ROTATION RESULT: PASS failures=0' in log
              and len(touches) == 4 and len(captured) == 4 and all(c['blue'] for c in captured)
              and not re.search(r'ROTATION .*FAIL', log))
    if args.soft_key:
        expected = [('1','0','528','320'), ('0','0','320','528')] * 2
        actual = re.findall(r'ROTATEKEY: mode=(\d+) result=(-?\d+) size=(\d+)x(\d+)', log)
        passed = code == 0 and actual == expected and log.count('CLASSPAD TOUCH: Rotate released') == 4
    result = dict(passed=passed, returncode=code, captures=captured, touches=touches,
                  inputs={name: dict(path=str(getattr(args, name).resolve()),
                                     sha256=hashlib.sha256(getattr(args, name).read_bytes()).hexdigest())
                          for name in ('emulator', 'binary', 'flash')})
    (args.output / 'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print('ROTATION HARNESS:', 'PASS' if passed else 'FAIL', flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
