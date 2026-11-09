#!/usr/bin/env python3
"""stage_bsp50.py - copy bsp/classpad50 to a PLATFORM\\CLASSPAD directory with
CRLF line endings.

build.exe (Platform Builder 5.0 and 6.0 alike) does not parse LF-only
`dirs` and `sources` files, and the other text files are happier as CRLF
on Windows too. A git checkout on Windows converts them through
.gitattributes; a copy from the Mac (to a shared folder or a mounted VM
disk) needs this script. Binary files (.dll, .exe, .ico, .2BP, .lnk, .bin,
.res, .png) are copied byte for byte; everything else is read as text,
normalised to LF and written with CRLF.

    tools/stage_bsp50.py /Volumes/WINCE500/PLATFORM/CLASSPAD
    tools/stage_bsp50.py --src bsp/classpad /Volumes/WINCE600/PLATFORM/CLASSPAD

The destination directory is created; text files already there are
overwritten, files that are not in the source are left alone, and a binary
file that already exists at the destination is kept (--force-binaries
overwrites it): FILES\\legacy-mfc\\mfcce300.dll in git is the original
1 KiB-aligned Pocket PC DLL, while the copy in PLATFORM\\CLASSPAD must be the
realigned one with the SH3CRT imports (tools/windows/claude-build/stage.py,
or a copy from the CE 6.0 tree's PLATFORM\\CLASSPAD\\FILES), like the Math
Xpander binaries in FILES\\xpander. --delete removes stale files.
"""
import argparse
import os
import shutil
import sys

BINARY = {'.dll', '.exe', '.ico', '.2bp', '.lnk', '.bin', '.res', '.png', '.bmp', '.cab'}
SKIP_DIRS = {'.git', '__pycache__'}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dest', help='PLATFORM\\CLASSPAD directory to write')
    ap.add_argument('--src', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'bsp', 'classpad50'))
    ap.add_argument('--delete', action='store_true', help='remove files in dest that are not in src')
    ap.add_argument('--force-binaries', action='store_true', help='overwrite binary files that already exist in dest')
    args = ap.parse_args()

    src = os.path.abspath(args.src)
    dest = os.path.abspath(args.dest)
    if not os.path.isdir(src):
        sys.exit(f'no such source directory: {src}')
    os.makedirs(dest, exist_ok=True)

    wanted = set()
    ntext = nbin = nkept = 0
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        rel = os.path.relpath(root, src)
        out_dir = dest if rel == '.' else os.path.join(dest, rel)
        os.makedirs(out_dir, exist_ok=True)
        for name in files:
            if name == '.DS_Store':
                continue
            s = os.path.join(root, name)
            d = os.path.join(out_dir, name)
            wanted.add(os.path.normcase(os.path.relpath(d, dest)))
            if os.path.splitext(name)[1].lower() in BINARY:
                if os.path.exists(d) and not args.force_binaries:
                    nkept += 1
                    continue
                shutil.copyfile(s, d)
                nbin += 1
                continue
            with open(s, 'rb') as f:
                data = f.read()
            if b'\0' in data:                   # a binary file with an unknown extension
                with open(d, 'wb') as f:
                    f.write(data)
                nbin += 1
                continue
            data = data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
            with open(d, 'wb') as f:
                f.write(data)
            ntext += 1

    removed = 0
    if args.delete:
        for root, dirs, files in os.walk(dest):
            for name in files:
                p = os.path.join(root, name)
                if os.path.normcase(os.path.relpath(p, dest)) not in wanted:
                    os.remove(p)
                    removed += 1
    print(f'{ntext} text files (CRLF) and {nbin} binary files written to {dest}, {nkept} existing binaries kept' + (f', {removed} stale files removed' if args.delete else ''))


if __name__ == '__main__':
    main()
