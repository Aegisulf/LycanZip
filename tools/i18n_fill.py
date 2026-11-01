#!/usr/bin/env python3
"""Fills the texts of a translation file from plain "id TAB text" files (for example a machine translation made in parts, or the corrections of a
reviewer), then runs i18n.py --update (which records the hash of the current English text for every text that has none) and --check.

  python tools/i18n_fill.py ja-JP part1.tsv part2.tsv ...

A line of the input files that is empty or starts with # is skipped. An id that is not in en-US.tsv is an error (nothing is written). A text
that is already there is replaced, and its old hash is dropped: the new text is the one made from the current English.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG_DIR = os.path.join(ROOT, "app", "Assets", "Lang")


def read_rows(path):
    raw = open(path, "rb").read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    nl = "\r\n" if "\r\n" in text else "\n"
    return bom, nl, text.split(nl)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    lang, inputs = sys.argv[1], sys.argv[2:]
    _, _, master = read_rows(os.path.join(LANG_DIR, "en-US.tsv"))
    ids = {l.split("\t", 1)[0] for l in master if l and not l.startswith("#")}
    new = {}
    for path in inputs:
        for n, line in enumerate(open(path, encoding="utf-8-sig").read().splitlines(), 1):
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2 or not parts[1]:
                print("%s:%d: not id TAB text" % (path, n))
                return 1
            if parts[0] not in ids:
                print("%s:%d: unknown id %s" % (path, n, parts[0]))
                return 1
            new[parts[0]] = parts[1]
    target = os.path.join(LANG_DIR, lang + ".tsv")
    bom, nl, rows = read_rows(target)
    seen = set()
    for i, row in enumerate(rows):
        if not row or row.startswith("#"):
            continue
        key = row.split("\t", 1)[0]
        if key in new:
            rows[i] = key + "\t" + new[key]
            seen.add(key)
    missing = set(new) - seen
    if missing:
        print("not in %s (run i18n.py --update first): %s" % (target, ", ".join(sorted(missing))))
        return 1
    open(target, "wb").write((b"\xef\xbb\xbf" if bom else b"") + nl.join(rows).encode("utf-8"))
    print("%s: %d texts filled" % (lang, len(new)))
    subprocess.call([sys.executable, os.path.join(ROOT, "tools", "i18n.py"), "--update", "--lang", lang])
    return 0


if __name__ == "__main__":
    sys.exit(main())
