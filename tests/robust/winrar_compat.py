#!/usr/bin/env python3
"""Passwords: LycanZip and WinRAR in both directions.

  python tests/robust/winrar_compat.py <lycanzip-cli.exe> [<WinRAR folder>]

A: LycanZip creates zip (AES-256, ZipCrypto) and 7z (with and without encrypted names), WinRAR extracts.
B: WinRAR creates RAR5, RAR5 with encrypted names and zip, LycanZip extracts (WinRAR 7 no longer writes RAR4).
Each with 12 passwords (spaces, punctuation, accents, CJK, emoji, 200 characters). A zip password that is not ASCII is
refused by LycanZip on purpose (7-Zip's engine writes zip passwords as single bytes), and so is a zip AES password of more
than 99 characters: those cells say "refused". A zip of WinRAR that LycanZip cannot open is tried with 7-Zip too: when 7-Zip
fails as well, the cell says "7-Zip too" (a limit of the engine: zip passwords go through the ANSI code page, AES ones stop at
99 characters). A double quote cannot be passed to WinRAR on its command line the way it reads it: only our own round trip.
Prints a table; the exit code is the number of cells that failed and were not expected to.
"""
import os
import shutil
import subprocess
import sys
import tempfile

PASSWORDS = [
    ("plain", "secret123"),
    ("space inside", "two words here"),
    ("single space", " "),
    ("punctuation", "^%&!$#@()[]{}<>|;,.=+-_~`"),
    ("single quote", "it's"),
    ("backslashes", "C:\\path\\to\\"),
    ("Latin accents", "\u00fcn\u00efc\u00f6d\u00e9 \u00e5\u00df"),
    ("Chinese", "\u4e2d\u6587\u5bc6\u78bc\u6e2c\u8a66"),
    ("Japanese", "\u65e5\u672c\u8a9e\u30d1\u30b9\u30ef\u30fc\u30c9"),
    ("emoji (non-BMP)", "pw\U0001F600\U0001F512end"),
    ("200 characters", "k" * 200),
    ("double quote", 'say "hi" now'),
]
CONTENT = b"the quick brown fox\r\n" * 50


def run(args, cwd=None):
    r = subprocess.run(args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    return r.returncode, r.stdout.decode("utf-8", "replace")


def main():
    cli = os.path.abspath(sys.argv[1])
    rardir = sys.argv[2] if len(sys.argv) > 2 else r"C:\Program Files\WinRAR"
    seven = r"C:\Program Files\7-Zip\7z.exe"
    winrar = os.path.join(rardir, "WinRAR.exe")   # extracts zip and 7z too (Rar.exe only knows RAR)
    rar = os.path.join(rardir, "Rar.exe")
    if not os.path.exists(winrar):
        print("WinRAR not installed: skipped")
        return 0
    work = tempfile.mkdtemp(prefix="lycan_winrar_")
    src = os.path.join(work, "src")
    os.makedirs(src)
    with open(os.path.join(src, "data.txt"), "wb") as f:
        f.write(CONTENT)

    def fresh(name):
        p = os.path.join(work, name)
        shutil.rmtree(p, ignore_errors=True)
        os.makedirs(p)
        return p

    def found(out):
        for root, _, files in os.walk(out):
            if "data.txt" in files:
                with open(os.path.join(root, "data.txt"), "rb") as f:
                    return f.read() == CONTENT
        return False

    ours = [  # label, extension, extra arguments of "a"
        ("zip AES-256", ".zip", []),
        ("zip ZipCrypto", ".zip", ["--profile", "compatible"]),
        ("7z", ".7z", []),
        ("7z enc. names", ".7z", ["--encrypt-names"]),
    ]
    theirs = [  # label, extension, tool, arguments before the archive name
        ("RAR5", ".rar", rar, ["a", "-ma5", "-ep1", "-idq"]),
        ("RAR5 enc. names", ".rar", rar, ["a", "-ma5", "-ep1", "-idq", "-hp{pw}"]),
        ("zip by WinRAR", ".zip", winrar, ["a", "-afzip", "-ep1", "-ibck", "-inul"]),
    ]
    unexpected = 0
    cols = [c[0] for c in ours] + [c[0] for c in theirs]
    print("%-17s" % "", " | ".join("%-15s" % c for c in cols))
    print("%-17s" % "", " | ".join("%-15s" % ("A: ours->WinRAR" if i < len(ours) else "B: WinRAR->ours") for i in range(len(cols))))
    for label, pw in PASSWORDS:
        cells = []
        ascii_pw = all(ord(c) < 128 for c in pw)
        for flabel, ext, extra in ours:
            arc = os.path.join(work, "a" + ext)
            if os.path.exists(arc):
                os.remove(arc)
            rc, out = run([cli, "a", arc, src, "-p", pw, "-y"] + extra)
            if rc != 0:
                expected = ext == ".zip" and (not ascii_pw or (len(pw) > 99 and not extra))
                cells.append("refused" if expected else "NOCREATE")
                if not expected:
                    unexpected += 1
                    print("   [%s %s] could not create: %s" % (label, flabel, out.strip()[:200]))
                continue
            if '"' in pw:
                rc, out = run([cli, "x", arc, "-o", fresh("outA"), "-p", pw, "-y"])
                ok = rc == 0 and found(os.path.join(work, "outA"))
                cells.append("own trip ok" if ok else "FAIL own trip")
                unexpected += 0 if ok else 1
                continue
            dest = fresh("outA")
            rc, out = run([winrar, "x", "-ibck", "-inul", "-y", "-p" + pw, arc, dest + "\\"])
            ok = rc == 0 and found(dest)
            cells.append("ok" if ok else "FAIL (%d)" % rc)
            if not ok:
                unexpected += 1
        for flabel, ext, tool, args in theirs:
            if '"' in pw:
                cells.append("-")
                continue
            arc = os.path.join(work, "b" + ext)
            if os.path.exists(arc):
                os.remove(arc)
            a = [x.replace("{pw}", pw) for x in args]
            if "-hp" + pw not in a:
                a.append("-p" + pw)
            rc, out = run([tool] + a + [arc, os.path.join(src, "data.txt")])
            if rc != 0 or not os.path.exists(arc):
                cells.append("NOCREATE (%d)" % rc)
                print("   [%s %s] WinRAR could not create: %s" % (label, flabel, out.strip()[:200]))
                continue
            dest = fresh("outB")
            rc, out = run([cli, "x", arc, "-o", dest, "-p", pw, "-y"])
            ok = rc == 0 and found(dest)
            if ok:
                cells.append("ok")
            elif ext == ".zip" and os.path.exists(seven) and run([seven, "x", "-y", "-p" + pw, "-o" + fresh("out7"), arc])[0] != 0:
                cells.append("7-Zip too")
            else:
                cells.append("FAIL (%d)" % rc)
                unexpected += 1
                print("   [%s %s] %s" % (label, flabel, out.strip()[:300]))
        print("%-17s" % label, " | ".join("%-15s" % c for c in cells))
    shutil.rmtree(work, ignore_errors=True)
    print("unexpected failures:", unexpected)
    return unexpected


if __name__ == "__main__":
    sys.exit(main())
