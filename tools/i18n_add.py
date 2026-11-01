#!/usr/bin/env python3
"""Adds many string ids to the catalog in one go, with their texts in all languages, and edits the source files that use them.

  python tools/i18n_add.py spec.py

spec.py (a Python file) defines
  REPL = [(file, old code, new code[, count])]   exact text replacements in the sources (file relative to the repository root); count
                                                  = how many times `old` must occur (default 1; 'all' = any number, at least one)
  CAT  = [(id, English, zh-TW, zh-CN, note)]     the new ids (an id that exists already is left as it is)
  MORE = {id: {'ja-JP': text, 'ko-KR': text, ...}} the texts of the other languages (every language of languages.h needs one, or
                                                  --check reports it as untranslated)
The ids go to the end of app/Assets/Lang/en-US.tsv (a section "## area" must exist already or the ids stay in the last one), the
translations to zh-TW.tsv and zh-CN.tsv and the files of MORE, then tools/i18n.py --update and --check run. Do not write backslashes inside a shell here-document
(the tool of Claude Code folds them): make the spec file with the Write tool.
"""
import importlib.util
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(path):
    spec = importlib.util.spec_from_file_location('spec', path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def read(p):
    raw = open(p, 'rb').read().decode('utf-8')
    nl = '\r\n' if '\r\n' in raw else '\n'
    return raw.replace('\r\n', '\n'), nl


def write(p, s, nl):
    open(p, 'wb').write(s.replace('\n', nl).encode('utf-8'))


def esc(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')


def main():
    m = load(sys.argv[1])
    for item in m.REPL:
        f, old, new = item[0], item[1], item[2]
        cnt = item[3] if len(item) > 3 else 1
        p = os.path.join(ROOT, f)
        s, nl = read(p)
        n = s.count(old)
        if n == 0 and new in s:
            continue   # applied already (a run that stopped half way)
        if n == 0:
            print('NOT FOUND in', f, ':', old[:80])
            sys.exit(1)
        if cnt != 'all' and n != cnt:
            print('COUNT', n, '!=', cnt, 'in', f, ':', old[:80])
            sys.exit(1)
        s = s.replace(old, new)
        write(p, s, nl)
    lang = os.path.join(ROOT, 'app', 'Assets', 'Lang')
    en, nl = read(os.path.join(lang, 'en-US.tsv'))
    have = {l.split('\t')[0] for l in en.split('\n')}
    add = [c for c in m.CAT if c[0] not in have]
    for c in m.CAT:
        if c[0] in have:
            old = [l for l in en.split('\n') if l.split('\t')[0] == c[0]][0].split('\t')[1]
            if old != esc(c[1]):
                print('ID EXISTS with other text:', c[0])
    if add:
        if not en.endswith('\n'):
            en += '\n'
        for c in add:
            en += '%s\t%s\t%s\n' % (c[0], esc(c[1]), c[4] if len(c) > 4 else '')
        write(os.path.join(lang, 'en-US.tsv'), en, nl)
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'i18n.py'), '--update'], stdout=subprocess.DEVNULL)
    more = getattr(m, 'MORE', {})
    codes = [('zh-TW', {c[0]: c[2] for c in add}), ('zh-CN', {c[0]: c[3] for c in add})]
    for code in sorted({code for texts in more.values() for code in texts}):
        codes.append((code, {k: texts[code] for k, texts in more.items() if code in texts and k in {c[0] for c in add}}))
    for code, tr in codes:
        p = os.path.join(lang, code + '.tsv')
        s, nl = read(p)
        out = []
        for l in s.split('\n'):
            k = l.split('\t')[0]
            if k in tr:
                l = k + '\t' + esc(tr[k]) + '\t'
            out.append(l)
        write(p, '\n'.join(out), nl)
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'i18n.py'), '--update'], stdout=subprocess.DEVNULL)
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'i18n.py'), '--check'], capture_output=True, text=True, encoding='utf-8')
    print(r.stdout[-600:])
    print('added', len(add), 'ids')


main()
