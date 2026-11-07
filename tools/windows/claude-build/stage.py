#!/usr/bin/env python3
"""stage.py - fill a claude-build package folder for the Platform Builder VM.

  stage.py <dest> [--platform <VM PLATFORM\\CLASSPAD>] [--xpander <unpacked cab>]
  (--xpander defaults to ~/Downloads/Xpander_SH3/files)

<dest> gets (see README.txt here):
  build.bat, README.txt, VERSION.txt
  src\\   bsp/classpad of the committed HEAD (git archive: CRLF as checked out)
         plus Math Xpander's files in FILES\\xpander (platform.bib puts them
         into the EXT region; they are not in the repository); every
         platform.bib module under FILES is realigned to 4 KiB sections
         (tools/pe4k.py: romimage takes no 1 KiB-aligned XIP module)
  orig\\  with --platform: the VM's current versions of every file src\\ changes
  hhk\\   wince-eact.hh3 and nkflash-eact.hh3 (make DEVICE=1 NK_WINDOW=eact hhk)
EXT.bin itself is made by makeimg in the VM (build.bat), next to NK.bin.
Nothing outside <dest> is written except the dbt/build-pkg build directory.
Commit first: src\\ comes from HEAD, not from the working tree.
"""
import io, os, shutil, subprocess, sys, tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import pe4k
BUILD = "build-pkg"

def run(*cmd, **kw):
    print("+", " ".join(cmd))
    return subprocess.run(cmd, check=True, cwd=kw.get("cwd", REPO), capture_output=kw.get("capture", False))


def main(argv):
    args = argv[1:]
    opts = {"--platform": None, "--xpander": os.path.expanduser("~/Downloads/Xpander_SH3/files")}
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
    cfg = open(os.path.join(src, "FILES", "config.bib"), "rb").read().decode()
    for line in ("NK          83860000  007A0000  RAMIMAGE", "EXT         80EE0000  00500000  RAMIMAGE"):
        if line not in cfg:
            sys.exit("stage: config.bib lacks %r (NK in the eActivity area, EXT in the hole)" % line)

    # Math Xpander: the cab's Program Files\Xpander and Windows files, flat
    xp = opts["--xpander"]
    if not os.path.isdir(os.path.join(xp, "Program Files", "Xpander")):
        sys.exit("stage: no Math Xpander at %s (--xpander: the unpacked cab)" % xp)
    dst = os.path.join(src, "FILES", "xpander")
    n = 0
    for sub in (os.path.join("Program Files", "Xpander"), "Windows"):
        for f in sorted(os.listdir(os.path.join(xp, sub))):
            p = os.path.join(xp, sub, f)
            if os.path.isfile(p) and f not in (".DS_Store", "regsvrce.exe"):
                shutil.copy2(p, os.path.join(dst, f)); n += 1
    bib = open(os.path.join(src, "FILES", "platform.bib"), "rb").read().decode()
    need = [l.split()[0] for l in bib.splitlines() if "\\FILES\\xpander\\" in l and not l.lstrip().startswith(";")]
    missing = [f for f in need if not os.path.isfile(os.path.join(dst, f))]
    if missing:
        sys.exit("stage: platform.bib wants %s, not in the Xpander files" % missing)
    print("stage: %d Math Xpander files in FILES\\xpander (%d bib entries)" % (n, len(need)))

    # MODULES from FILES (the Pocket PC SH3 binaries, linked with 1 KiB
    # SectionAlignment): 4 KiB sections for romimage, checked by pe4k.
    sect, n = None, 0
    for l in bib.splitlines():
        w = l.split()
        if not w or w[0].startswith(";"):
            continue
        if len(w) == 1 and w[0] in ("MODULES", "FILES"):
            sect = w[0]; continue
        if sect == "MODULES" and len(w) >= 2 and "\\PLATFORM\\CLASSPAD\\FILES\\" in w[1]:
            p = os.path.join(src, *w[1].split("\\PLATFORM\\CLASSPAD\\", 1)[1].split("\\"))
            data = open(p, "rb").read()
            try:
                new, what = pe4k.realign(data)
                errs = pe4k.check(data, new)
            except pe4k.PEError as e:
                sys.exit("stage: %s: %s" % (w[0], e))
            if errs:
                sys.exit("stage: %s: %s" % (w[0], "; ".join(errs[:5])))
            if new != data:
                open(p, "wb").write(new); n += 1
    print("stage: %d modules realigned to 4 KiB sections" % n)

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

    for f in ("build.bat", "README.txt"):
        shutil.copy(os.path.join(HERE, f), os.path.join(dest, f))
    head = run("git", "log", "-1", "--format=%H%n%ci%n%s", capture=True).stdout.decode()
    branch = run("git", "rev-parse", "--abbrev-ref", "HEAD", capture=True).stdout.decode().strip()
    open(os.path.join(dest, "VERSION.txt"), "wb").write((head + "branch " + branch + "\n").replace("\n", "\r\n").encode())
    print("stage: package in", dest)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
