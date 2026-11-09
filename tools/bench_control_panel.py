#!/usr/bin/env python3
"""Reproduce Control Panel -> System -> close Control Panel, then probe Memory.

Samples begin at the close press. Repeated Memory taps test responsiveness;
the final frame must be inspected/compared, not treated as an automatic pass.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil

p = argparse.ArgumentParser(description=__doc__)
for key in ('emulator', 'binary', 'flash', 'output'):
    p.add_argument('--' + key, type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
presses = [(14,515,2000000), (55,436,2100000), (163,437,2200000),
           (110,183,3000000), (110,183,3008000), (306,13,4000000)]
presses += [(122,141,t) for t in range(4100000, 6800000, 300000)]
env = dict(os.environ, CALCEMU_MODEL='fx-cp400', SDL_VIDEODRIVER='dummy',
           SDL_RENDER_DRIVER='software', HLE_RTC_RUNNING='1', HLE_FBDUMP_ALL='1',
           HLE_FBDUMP=str((a.output/'final.ppm').resolve()),
           HLE_OSROM=str(a.flash.resolve()), HLE_OSROM_PAD='0x00EE0000',
           HLE_CB_TOUCHSEQ=';'.join(f'{x},{y},{t},4000' for x,y,t in presses),
           HLE_DBT_SAMPLE='4000000', HLE_EXIT_AT='7000000')
env.pop('HLE_FB_EXPECT', None)
with (a.output/'run.log').open('w') as log:
    with subprocess.Popen([str(a.emulator.resolve()), str(a.binary.resolve())],
                          env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True) as process:
        for line in process.stdout:
            log.write(line); log.flush()
            if 'INJECTTOUCHSEQ down' in line and 'at tick 4000000 ' in line:
                shutil.copyfile(a.output/'final.ppm', a.output/'before-close.ppm')
        code = process.wait()
(a.output/'inputs.json').write_text(json.dumps(dict(
    returncode=code, presses=presses, sample_start=4000000,
    sample_end=7000000, final_frame_verified=False,
    inputs={key: {'path':str(getattr(a,key).resolve()),
                  'sha256':hashlib.sha256(getattr(a,key).read_bytes()).hexdigest()}
            for key in ('emulator','binary','flash')}), indent=2)+'\n')
raise SystemExit(code)
