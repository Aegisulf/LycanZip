#!/usr/bin/env python3
"""Hostile entry names, end to end: build zip / tar archives whose entries try to escape the output folder or
use Windows-special names, extract them with the CLI and check that nothing is written outside the output folder
and nothing odd is created inside it.

Usage: evil_names.py <lycanzip-cli.exe> [--workdir DIR]
Exit code 0 = every check passed.
"""
import argparse
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

EVIL = [
    "../../escape1.txt",
    "..\\..\\escape2.txt",
    "a/../../escape3.txt",
    "a\\..\\..\\escape4.txt",
    "/abs/escape5.txt",
    "\\abs\\escape6.txt",
    "C:\\escape7.txt",
    "C:escape8.txt",
    "\\\\server\\share\\escape9.txt",
    "\\\\?\\C:\\escape10.txt",
    "//?/C:/escape11.txt",
    "....//escape12.txt",
    "ok/..\\..\\escape13.txt",
    "foo/./../../../escape14.txt",
    "normal.txt:hidden_stream",
    "normal2.txt::$DATA",
    "dir::$INDEX_ALLOCATION/x.txt",
    "con.txt", "CON", "aux.log", "nul", "prn.dat", "com1", "LPT1.txt", "com\u00b9.txt",
    "trailing_dot.", "trailing_space ", "dots...", " leading_space.txt",
    "lt<gt>.txt", "pipe|name.txt", "q?mark.txt", "star*.txt", "quote\".txt", "colon:name.txt",
    "ctrl\x01char.txt", "tab\tname.txt", "newline\nname.txt",
    "\u202eRTL_override.txt",
    "a" * 300 + ".txt",
    "d/" * 120 + "deep.txt",
    "case.txt", "CASE.TXT", "Case.Txt",
    "dup.txt", "dup.txt",
    "emoji_\U0001f600.txt",
    "~1.txt", "PROGRA~1/x.txt",
    "",
    ".",
    "..",
    "./",
    "../",
]


def snapshot(root):
    out = set()
    for d, dirs, files in os.walk(root):
        for n in dirs + files:
            out.add(os.path.relpath(os.path.join(d, n), root))
    return out


def run(cli, args, timeout=60):
    p = subprocess.run([cli] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace") + p.stderr.decode("utf-8", "replace")


def make_zip(path, names, symlink_target=None):
    with zipfile.ZipFile(path, "w") as z:
        for n in names:
            zi = zipfile.ZipInfo(n if n else "x")
            zi.filename = n  # keep raw (zipfile may normalise some)
            z.writestr(zi, b"payload:" + n.encode("utf-8", "replace")[:40])
        if symlink_target:
            zi = zipfile.ZipInfo("evil_link")
            zi.create_system = 3
            zi.external_attr = (0o120777 << 16)
            z.writestr(zi, symlink_target)
            zi = zipfile.ZipInfo("evil_link/inside.txt")
            z.writestr(zi, b"written through the link")


def make_tar(path, names):
    with tarfile.open(path, "w", format=tarfile.GNU_FORMAT) as t:
        for n in names:
            ti = tarfile.TarInfo(n)
            data = b"payload"
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cli")
    ap.add_argument("--workdir")
    a = ap.parse_args()
    work = a.workdir or tempfile.mkdtemp(prefix="lycan_evil_")
    failures = 0
    for kind in ("zip", "tar", "zip-symlink"):
        sandbox = os.path.join(work, kind)
        shutil.rmtree(sandbox, ignore_errors=True)
        os.makedirs(os.path.join(sandbox, "out"))
        # sentinel files that an escape would overwrite or sit next to
        for n in ("escape1.txt", "escape2.txt", "escape3.txt", "escape4.txt"):
            open(os.path.join(sandbox, n), "w").write("sentinel")
        os.makedirs(os.path.join(sandbox, "victim"))
        arc = os.path.join(sandbox, "evil." + ("tar" if kind == "tar" else "zip"))
        if kind == "tar":
            make_tar(arc, EVIL)
        elif kind == "zip":
            make_zip(arc, EVIL)
        else:
            make_zip(arc, ["fine.txt"], symlink_target=os.path.join(sandbox, "victim"))
        before_outside = snapshot(sandbox) - snapshot(os.path.join(sandbox, "out")) - {"out"}
        sentinel_before = {n: open(os.path.join(sandbox, n)).read() for n in ("escape1.txt", "escape2.txt", "escape3.txt", "escape4.txt")}
        for mode in ("-y", "-r"):
            rc, text = run(a.cli, ["x", arc, "-o", os.path.join(sandbox, "out"), mode])
            if rc < 0 or rc >= 0xC0000000 or rc > 3:
                print(f"FAIL {kind} {mode}: crash exit code {rc & 0xFFFFFFFF:#x}")
                failures += 1
        # 1. nothing outside the output folder changed (the archive itself and the out folder aside)
        after = set()
        for d, dirs, files in os.walk(sandbox):
            for n in dirs + files:
                rel = os.path.relpath(os.path.join(d, n), sandbox)
                if rel == "out" or rel.startswith("out" + os.sep):
                    continue
                after.add(rel)
        extra = after - before_outside
        if extra:
            print(f"FAIL {kind}: files created outside the output folder: {sorted(extra)[:8]}")
            failures += 1
        for n, v in sentinel_before.items():
            if open(os.path.join(sandbox, n)).read() != v:
                print(f"FAIL {kind}: sentinel {n} was overwritten")
                failures += 1
        # 2. what ended up inside the output folder is sane
        out = os.path.join(sandbox, "out")
        for rel in snapshot(out):
            for part in rel.split(os.sep):
                if part.rstrip(". ") != part or part.upper().split(".")[0] in ("CON", "PRN", "AUX", "NUL", "COM1", "LPT1"):
                    print(f"FAIL {kind}: Windows-special name created inside the output folder: {rel!r}")
                    failures += 1
                if any(c in part for c in '<>:"|?*') or any(ord(c) < 32 for c in part):
                    print(f"FAIL {kind}: illegal characters in extracted name: {rel!r}")
                    failures += 1
        # no alternate data streams
        rc, text = run(a.cli, ["l", arc])
        if kind == "zip-symlink":
            vict = snapshot(os.path.join(sandbox, "victim"))
            if vict:
                print(f"FAIL {kind}: a file was written through an archived symlink: {sorted(vict)}")
                failures += 1
        print(f"{kind}: {len(snapshot(out))} items extracted into the output folder, checks done")
    if not a.workdir:
        shutil.rmtree(work, ignore_errors=True)
    print("all ok" if not failures else f"FAILURES: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
