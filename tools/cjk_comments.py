#!/usr/bin/env python3
"""Lists the lines whose COMMENT (after // or #, or a whole line of a block comment / markup comment) has Chinese text.
Development aid for the clean-up of the public files. Usage: cjk_comments.py FILE..."""
import re
import sys

CJK = re.compile(r'[一-鿿　-〿＀-￯]')


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    for p in sys.argv[1:]:
        marker = '#' if p.endswith(('.ps1', '.py', '.txt', '.cmake', '.yml', '.yaml', '.in', '.tsv')) else '//'
        try:
            lines = open(p, encoding='utf-8-sig').read().split(chr(10))
        except Exception:
            continue
        hits = []
        for i, l in enumerate(lines):
            if not CJK.search(l):
                continue
            k = l.rfind(marker)
            if (k >= 0 and CJK.search(l, k)) or l.lstrip().startswith(('*', '<!--', '/*')) or ('<!--' in l):
                hits.append((i + 1, l.rstrip()))
        if hits:
            print('=== %s (%d)' % (p, len(hits)))
            for n, l in hits:
                print('%d\t%s' % (n, l))


main()
