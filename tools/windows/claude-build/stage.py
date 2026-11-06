#!/usr/bin/env python3
"""stage.py - fill a claude-build package folder for the Platform Builder VM.

  stage.py <dest> [--platform <VM PLATFORM\\CLASSPAD>] [--xpander <unpacked cab>]

<dest> gets (see README.txt here):
  build.bat, README.txt, VERSION.txt
  src\\   bsp/classpad of the committed HEAD (git archive: CRLF as checked out),
         with FILES\\config.bib switched to the eActivity window (the VM builds
         its NK image for 83860000, the repository default is the hole window)
  orig\\  with --platform: the VM's current versions of every file src\\ changes
  hhk\\   wince-eact.hh3 and nkflash-eact.hh3 (make DEVICE=1 NK_WINDOW=eact hhk)
         and, with --xpander, EXT.bin (make NK_WINDOW=eact extrom)
Nothing outside <dest> is written except the dbt/build-pkg build directory.
Commit first: src\\ comes from HEAD, not from the working tree.
"""
import io, os, shutil, subprocess, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BUILD = "build-pkg"

EACT = [
    ("    NK          81000000  003E0000  RAMIMAGE    ; 3.875 MiB", "\0A"),
    (";   NK          83860000  007A0000  RAMIMAGE    ; alternative: the eActivity area (make NK_WINDOW=eact; docs/03: needs an OS patch first)",
     "    NK          83860000  007A0000  RAMIMAGE    ; the eActivity area (make NK_WINDOW=eact; reformatted by Casio's OS at every boot, docs/03)"),
    ("\0A", ";   NK          81000000  003E0000  RAMIMAGE    ; 3.875 MiB"),
    ("    ROMSTART=81000000\r\n    ROMSIZE=003E0000\r\n;   ROMSTART=83860000  ROMSIZE=007A0000           ; the eActivity-area variant (with the NK line above)",
     ";   ROMSTART=81000000  ROMSIZE=003E0000           ; the hole window (with the NK line above)\r\n    ROMSTART=83860000\r\n    ROMSIZE=007A0000"),
]


def run(*cmd, **kw):
    print("+", " ".join(cmd))
    return subprocess.run(cmd, check=True, cwd=kw.get("cwd", REPO), capture_output=kw.get("capture", False))


def main(argv):
    args = argv[1:]
    opts = {"--platform": None, "--xpander": None}
    pos = []
    while args:
        a = args.pop(0)
        if a in opts:
            opts[a] = args.pop(0)
        else:
            pos.append(a)
    if len(pos) != 1:
        print(__doc__); return 2
    dest = os.path.abspath(pos[0])
    src, orig, hhk = (os.path.join(dest, d) for d in ("src", "orig", "hhk"))
    for d in (src, orig, hhk):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)

    tar = run("git", "archive", "HEAD", "bsp/classpad", capture=True).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        for m in t.getmembers():
            rel = m.name.split("/", 2)[2] if m.name.count("/") >= 2 else ""
            if not rel or not m.isfile():
                continue
            out = os.path.join(src, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            open(out, "wb").write(t.extractfile(m).read())
    cfg = os.path.join(src, "FILES", "config.bib")
    s = open(cfg, "rb").read().decode()
    for a, b in EACT:
        if a not in s:
            sys.exit("stage: config.bib changed, update the eActivity switch (%r)" % a[:40])
        s = s.replace(a, b, 1)
    open(cfg, "wb").write(s.encode())

    if opts["--platform"]:
        n = 0
        for root, _, files in os.walk(src):
            for f in files:
                rel = os.path.relpath(os.path.join(root, f), src)
                have = os.path.join(opts["--platform"], rel)
                if os.path.isfile(have) and open(have, "rb").read() != open(os.path.join(root, f), "rb").read():
                    os.makedirs(os.path.dirname(os.path.join(orig, rel)), exist_ok=True)
                    shutil.copy2(have, os.path.join(orig, rel)); n += 1
        print("stage: %d VM files saved in orig" % n)

    dbt = os.path.join(REPO, "dbt")
    run("make", "DEVICE=1", "NK_WINDOW=eact", "BUILD=" + BUILD, "hhk", cwd=dbt)
    shutil.copy(os.path.join(dbt, BUILD, "wince.hh3"), os.path.join(hhk, "wince-eact.hh3"))
    shutil.copy(os.path.join(dbt, BUILD, "nkflash.hh3"), os.path.join(hhk, "nkflash-eact.hh3"))
    if opts["--xpander"]:
        run("make", "NK_WINDOW=eact", "BUILD=" + BUILD, "XPANDER=" + os.path.abspath(opts["--xpander"]), "extrom", cwd=dbt)
        shutil.copy(os.path.join(dbt, BUILD, "EXT.bin"), os.path.join(hhk, "EXT.bin"))

    for f in ("build.bat", "README.txt"):
        shutil.copy(os.path.join(HERE, f), os.path.join(dest, f))
    head = run("git", "log", "-1", "--format=%H%n%ci%n%s", capture=True).stdout.decode()
    branch = run("git", "rev-parse", "--abbrev-ref", "HEAD", capture=True).stdout.decode().strip()
    open(os.path.join(dest, "VERSION.txt"), "wb").write((head + "branch " + branch + "\n").replace("\n", "\r\n").encode())
    print("stage: package in", dest)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
