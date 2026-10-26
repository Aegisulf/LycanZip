#!/usr/bin/env python3
"""LycanZip localization tool.

Every user-visible string has an id (for example "main.extract_here"). The code and the XAML only contain ids; the texts live in
app/Assets/Lang/<language>.tsv (see src/core/include/lycan/i18n.h):
  en-US.tsv   the master: id TAB English text TAB note for the translators (optional)
  <lang>.tsv  a translation: id TAB text TAB hash of the English text it was made from (8 hex digits, filled in by this tool)

  --check            every id used in the code exists in en-US, every id of en-US is used, every translation has all ids, no stale ids,
                     the {0} placeholders match, and no translation is older than its English text (the hash differs: "needs review").
                     Exit code 1 when something is wrong.
  --update           rewrite the translation files: new ids get an empty line, ids that are gone are dropped, a text without hash gets the
                     current hash
  --review           list the translations whose English text changed since they were made (--lang to pick one)
  --accept IDS...    after a translation was looked at (or changed): set the hash of these ids to the current one (--all: every id)
  --rename OLD NEW   change an id everywhere (code, XAML, all language files)
  --list             the ids of the master, in order
  --en-table         write docs/EN-REVIEW.md: the English texts with notes and the state of their review (for a native speaker)
  --en-accept IDS... record that the English of these ids was reviewed (--all: every id); later changes show up as "changed"
  --lint             XAML texts and C++ string literals that look like UI text but are not an id (a text that should have been translated)

Where an id is used: XAML attribute values and texts that equal an id; C++ string literals that equal an id (the first argument of Tr / Tf
for sure). A line that ends with // i18n:skip is not looked at by --lint. Option --lang en-US,zh-CN limits the languages.
"""
import argparse
import glob
import hashlib
import io
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG_DIR = os.path.join(ROOT, "app", "Assets", "Lang")
SOURCE = "en-US"
ID_RE = re.compile(r"^[a-z][a-z0-9_]*(\.[a-z0-9_]+)+$")
PLACEHOLDER = re.compile(r"\{(\d+)(?::[^{}]*)?\}")
XAML_TEXT_ATTRS = {"Text", "Content", "ToolTipService.ToolTip", "Header", "PlaceholderText", "AutomationProperties.Name", "Title", "Message",
                   "Description", "PrimaryButtonText", "SecondaryButtonText", "CloseButtonText", "Label"}


# ---------------------------------------------------------------- C++ literals

def decode_cpp(body):
    out = []
    i = 0
    simple = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0", "a": "\a", "b": "\b", "f": "\f", "v": "\v"}
    while i < len(body):
        c = body[i]
        if c != "\\" or i + 1 >= len(body):
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        if n == "u" and re.fullmatch(r"[0-9a-fA-F]{4}", body[i + 2:i + 6]):
            out.append(chr(int(body[i + 2:i + 6], 16)))
            i += 6
        elif n == "U" and re.fullmatch(r"[0-9a-fA-F]{8}", body[i + 2:i + 10]):
            out.append(chr(int(body[i + 2:i + 10], 16)))
            i += 10
        elif n in simple:
            out.append(simple[n])
            i += 2
        else:
            out.append(n)
            i += 2
    return "".join(out)


def cpp_literals(path):
    """[(line, decoded text, start offset, call)] of the string literals outside comments (adjacent literals merged); call = the word before
    the '(' that precedes the literal ('Tr', 'Tf', ...) or ''. Lines ending with i18n:skip are left out."""
    src = open(path, encoding="utf-8-sig").read()
    lines = src.split("\n")
    n = len(src)
    i = 0
    line = 1
    toks = []
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += src.count("\n", i, j)
            i = j
        elif c == "R" and src.startswith('R"', i) or (c in "LuU" and src.startswith('R"', i + 1)) or src.startswith('u8R"', i):
            m = re.compile(r'(?:u8|[LuU])?R"([^(\s]*)\(').match(src, i)
            if not m:
                i += 1
                continue
            end = src.find(")" + m.group(1) + '"', m.end())
            end = n if end < 0 else end
            toks.append((line, src[m.end():end], end + len(m.group(1)) + 2, i))
            line += src.count("\n", i, end)
            i = end + len(m.group(1)) + 2
        elif c == '"' or (c in "LuU" and src.startswith('"', i + 1) and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == "_"))) or src.startswith('u8"', i):
            start = i
            i = src.index('"', i) + 1
            body_start = i
            while i < n and src[i] != '"':
                if src[i] == "\\":
                    i += 1
                if src[i] == "\n":
                    break
                i += 1
            toks.append((line, decode_cpp(src[body_start:i]), i + 1, start))
            i += 1
        elif c == "'":
            i += 1
            while i < n and src[i] != "'":
                if src[i] == "\\":
                    i += 1
                i += 1
            i += 1
        else:
            i += 1
    merged = []
    for t in toks:
        if merged:
            gap = src[merged[-1][2]:t[3]]
            if re.fullmatch(r"(\s|//[^\n]*\n|/\*.*?\*/)*", gap, re.S):
                prev = merged[-1]
                merged[-1] = (prev[0], prev[1] + t[1], t[2], prev[3])
                continue
        merged.append(t)
    res = []
    for ln, text, _, start in merged:
        if "i18n:skip" in lines[ln - 1]:
            continue
        m = re.search(r"(\w+)\s*\(\s*$", src[max(0, start - 40):start])
        res.append((ln, text, start, m.group(1) if m else ""))
    return res


def xaml_values(path):
    """[(attribute name or '#text', value)] of the XAML file."""
    root = ET.parse(path).getroot()
    res = []

    def visit(el):
        for k, v in el.attrib.items():
            res.append((k.split("}")[-1], v))
        if el.text and el.text.strip():
            res.append(("#text", el.text.strip()))
        for ch in el:
            visit(ch)
            if ch.tail and ch.tail.strip():
                res.append(("#text", ch.tail.strip()))

    visit(root)
    return res


def source_files():
    f = sorted(glob.glob(os.path.join(ROOT, "app", "*.xaml")))
    c = sorted(glob.glob(os.path.join(ROOT, "app", "*.cpp")) + glob.glob(os.path.join(ROOT, "app", "*.h")) +
               glob.glob(os.path.join(ROOT, "src", "core", "*.cpp")) + glob.glob(os.path.join(ROOT, "src", "cli", "*.cpp")) + glob.glob(os.path.join(ROOT, "shellext", "*.cpp")) + glob.glob(os.path.join(ROOT, "src", "core", "include", "lycan", "*.h")))
    return f, c


# ---------------------------------------------------------------- language files

def esc(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")


def unesc(s):
    out = []
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s) and s[i + 1] in "nt\\":
            out.append({"n": "\n", "t": "\t", "\\": "\\"}[s[i + 1]])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def text_hash(english):
    return hashlib.sha1(english.encode("utf-8")).hexdigest()[:8]


def read_lang(path):
    """(entries, raw lines): entries = {id: (text, third column)}, in file order."""
    entries = {}
    if not os.path.exists(path):
        return entries, []
    lines = open(path, encoding="utf-8-sig").read().split("\n")
    for line in lines:
        line = line.rstrip("\r")
        if not line or line.startswith("#") or "\t" not in line:
            continue
        parts = line.split("\t")
        if parts[0]:
            entries[parts[0]] = (unesc(parts[1]) if len(parts) > 1 else "", parts[2] if len(parts) > 2 else "")
    return entries, lines


def write_lines(path, lines):
    while lines and lines[-1] == "":
        lines.pop()
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(l.rstrip("\r") for l in lines) + "\n")


def langs_of(arg):
    if arg:
        return arg.split(",")
    return sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(LANG_DIR, "*.tsv")))


def core_inc_text(master):
    """The English texts of the ids that start with "core." as a C++ table for src/core/core_messages.inc (compiled into the Core, so
    logs, the CLI and the tests have English without any file; see lycan/message.h)."""
    def lit(t):
        out = []
        for ch in t:
            o = ord(ch)
            if ch == "\\": out.append("\\\\")
            elif ch == '"': out.append('\\"')
            elif ch == "\n": out.append("\\n")
            elif ch == "\t": out.append("\\t")
            elif o < 32: out.append("\\x%02x" % o)
            elif o < 128: out.append(ch)
            elif o < 0x10000: out.append("\\u%04x" % o)
            else:
                o -= 0x10000
                out.append("\\u%04x\\u%04x" % (0xD800 + (o >> 10), 0xDC00 + (o & 0x3FF)))
        return "".join(out)
    lines = ["// Generated by tools/i18n.py from app/Assets/Lang/en-US.tsv (ids that start with core.). Do not edit."]
    for k, (t, _) in master.items():
        if k.startswith("core."):
            lines.append('{L"%s", L"%s"},' % (k, lit(t)))
    return "\n".join(lines) + "\n"


def cpp_lit(t):
    out = []
    for ch in t:
        o = ord(ch)
        if ch == "\\": out.append("\\\\")
        elif ch == '"': out.append('\\"')
        elif ch == "\n": out.append("\\n")
        elif ch == "\t": out.append("\\t")
        elif o < 32: out.append("\\x%02x" % o)
        elif o < 128: out.append(ch)
        elif o < 0x10000: out.append("\\u%04x" % o)
        else:
            o -= 0x10000
            out.append("\\u%04x\\u%04x" % (0xD800 + (o >> 10), 0xDC00 + (o & 0x3FF)))
    return "".join(out)


SHELL_INC = os.path.join(ROOT, "shellext", "shell_texts.inc")


LANGUAGES_H = os.path.join(ROOT, "src", "core", "include", "lycan", "languages.h")


def registered_languages():
    """The language codes of lycan::kLanguages (src/core/include/lycan/languages.h), in that order."""
    text = open(LANGUAGES_H, encoding="utf-8").read()
    return re.findall(r'\{L"([A-Za-z]{2,3}(?:-[A-Za-z0-9]+)*)", L"lang\.[a-z0-9_]+"\}', text)


def shell_inc_text(master):
    """The texts of the context menu (ids that start with shell.) in every language of languages.h, compiled into the shell extension DLL: it
    runs inside Explorer and must not read language files. Row: id, then one text for each language in the order of kLanguages."""
    codes = registered_languages()
    tables = {}
    for code in codes:
        tables[code] = {} if code == SOURCE else read_lang(os.path.join(LANG_DIR, code + ".tsv"))[0]
    lines = ["// Generated by tools/i18n.py from app/Assets/Lang (ids that start with shell.). Do not edit. Row: id, then the text in each language of lycan::kLanguages: " + ", ".join(codes) + "."]
    for k, (t, _) in master.items():
        if k.startswith("shell."):
            cols = [cpp_lit(t if code == SOURCE else (tables[code].get(k, ("", ""))[0] or t)) for code in codes]
            lines.append('{L"%s", {%s}},' % (k, ", ".join('L"%s"' % c for c in cols)))
    return "\n".join(lines) + "\n"


def read_shell_inc():
    try:
        return open(SHELL_INC, encoding="utf-8", newline="").read().replace("\r\n", "\n")
    except OSError:
        return None


CORE_INC = os.path.join(ROOT, "src", "core", "core_messages.inc")


def read_core_inc():
    try:
        return open(CORE_INC, encoding="utf-8", newline="").read().replace("\r\n", "\n")
    except OSError:
        return None


# ---------------------------------------------------------------- where ids are used

def used_ids(master):
    """({id: [places]}, [(place, text)] of Tr / Tf arguments that are not an id of the master)"""
    used = {}
    unknown = []
    xamls, cpps = source_files()
    for f in xamls:
        for attr, v in xaml_values(f):
            if v in master:
                used.setdefault(v, []).append(os.path.basename(f))
            elif ID_RE.match(v) and attr in XAML_TEXT_ATTRS:
                unknown.append((os.path.basename(f), v))
    for f in cpps:
        for ln, text, _, call in cpp_literals(f):
            if text in master:
                used.setdefault(text, []).append("%s:%d" % (os.path.basename(f), ln))
            elif call in ("Tr", "Tf"):
                unknown.append(("%s:%d" % (os.path.basename(f), ln), text))
    return used, unknown


# ---------------------------------------------------------------- main

def cmd_check(a):
    master, _ = read_lang(os.path.join(LANG_DIR, SOURCE + ".tsv"))
    used, unknown = used_ids(master)
    bad = 0
    print("%s: %d ids" % (SOURCE, len(master)))
    for place, t in unknown[:30]:
        print("  not an id of the master: %s  (%s)" % (t[:60], place))
    bad += len(unknown)
    if read_core_inc() != core_inc_text(master):
        print("  src/core/core_messages.inc is out of date: run --update"); bad += 1
    if sorted(registered_languages()) != sorted(langs_of(None)):
        print("  the languages of src/core/include/lycan/languages.h (%s) and the files of app/Assets/Lang (%s) differ" % (", ".join(registered_languages()), ", ".join(langs_of(None)))); bad += 1
    elif read_shell_inc() != shell_inc_text(master):
        print("  shellext/shell_texts.inc is out of date: run --update"); bad += 1
    unused = [k for k in master if k not in used]
    for k in unused[:30]:
        print("  unused id: %s" % k)
    bad += len(unused)
    for k, (t, _) in master.items():
        if not t:
            print("  empty English text: %s" % k); bad += 1
    for lang in langs_of(a.lang):
        if lang == SOURCE:
            continue
        tr, _ = read_lang(os.path.join(LANG_DIR, lang + ".tsv"))
        stale = [k for k in tr if k not in master]
        missing = [k for k in master if not tr.get(k, ("", ""))[0]]
        mism = [k for k in master if tr.get(k, ("", ""))[0] and sorted(set(PLACEHOLDER.findall(master[k][0]))) != sorted(set(PLACEHOLDER.findall(tr[k][0])))]
        review = [k for k in master if tr.get(k, ("", ""))[0] and tr[k][1] != text_hash(master[k][0])]
        print("%s: %d ids, %d untranslated, %d stale, %d placeholder mismatches, %d need review" % (lang, len(master), len(missing), len(stale), len(mism), len(review)))
        for k in stale[:20]:
            print("  stale: %s" % k)
        for k in mism[:20]:
            print("  placeholder mismatch: %s  ->  %s" % (k, tr[k][0]))
        if not a.allow_missing:
            for k in missing[:20]:
                print("  missing: %s" % k)
        for k in review[:20]:
            print("  needs review (English changed): %s" % k)
        bad += len(stale) + len(mism) + len(review) + (0 if a.allow_missing else len(missing))
    return 1 if bad else 0


def cmd_update(a):
    master, mlines = read_lang(os.path.join(LANG_DIR, SOURCE + ".tsv"))
    want = core_inc_text(master)
    if read_core_inc() != want:
        open(CORE_INC, "w", encoding="utf-8", newline="\n").write(want)
        print("core_messages.inc: updated")
    for lang in langs_of(a.lang):
        if lang == SOURCE:
            continue
        path = os.path.join(LANG_DIR, lang + ".tsv")
        tr, lines = read_lang(path)
        head = [l.rstrip("\r") for l in lines if l.startswith("#") and not l.startswith("##")][:1]
        out = head[:] if head else ["# LycanZip " + lang + ": id TAB text TAB hash of the English text. Maintained with tools/i18n.py."]
        section = None
        for mline in mlines:
            mline = mline.rstrip("\r")
            if mline.startswith("##"):
                out.append(mline)
                continue
            if not mline or mline.startswith("#") or "\t" not in mline:
                continue
            k = mline.split("\t")[0]
            t, h = tr.get(k, ("", ""))
            if t and not h:
                h = text_hash(master[k][0])
            out.append("%s\t%s\t%s" % (k, esc(t), h if t else ""))
        stale = [k for k in tr if k not in master]
        write_lines(path, out)
        print("%s: updated (%d ids, %d untranslated, %d stale removed)" % (lang, len(master), sum(1 for k in master if not tr.get(k, ("", ""))[0]), len(stale)))
    want_shell = shell_inc_text(master)   # after the translation files: the texts of the menu come from all of them
    if read_shell_inc() != want_shell:
        open(SHELL_INC, "w", encoding="utf-8", newline="\n").write(want_shell)
        print("shell_texts.inc: updated")
    return 0


def cmd_review(a):
    master, _ = read_lang(os.path.join(LANG_DIR, SOURCE + ".tsv"))
    n = 0
    for lang in langs_of(a.lang):
        if lang == SOURCE:
            continue
        tr, _ = read_lang(os.path.join(LANG_DIR, lang + ".tsv"))
        for k in master:
            if tr.get(k, ("", ""))[0] and tr[k][1] != text_hash(master[k][0]):
                print("%s  %s\n    English: %s\n    %s: %s" % (lang, k, master[k][0], lang, tr[k][0])); n += 1
    print("%d to review" % n)
    return 0


def cmd_accept(a):
    master, _ = read_lang(os.path.join(LANG_DIR, SOURCE + ".tsv"))
    ids = list(master) if a.all else a.accept
    for lang in langs_of(a.lang):
        if lang == SOURCE:
            continue
        path = os.path.join(LANG_DIR, lang + ".tsv")
        tr, lines = read_lang(path)
        out = []
        for line in lines:
            line = line.rstrip("\r")
            if line and not line.startswith("#") and "\t" in line:
                p = line.split("\t")
                if p[0] in ids and p[0] in master and len(p) > 1 and p[1]:
                    p = (p + ["", ""])[:3]
                    p[2] = text_hash(master[p[0]][0])
                    line = "\t".join(p)
            out.append(line)
        write_lines(path, out)
    print("accepted")
    return 0


def cmd_rename(a):
    old, new = a.rename
    if not ID_RE.match(new):
        print("not a valid id: " + new); return 1
    master, _ = read_lang(os.path.join(LANG_DIR, SOURCE + ".tsv"))
    if old not in master or new in master:
        print("unknown id %s or %s exists already" % (old, new)); return 1
    for p in glob.glob(os.path.join(LANG_DIR, "*.tsv")):
        s = open(p, encoding="utf-8-sig", newline="").read()
        s2 = re.sub(r"(?m)^%s\t" % re.escape(old), new + "\t", s)
        if s2 != s:
            open(p, "w", encoding="utf-8", newline="").write(s2)
    xamls, cpps = source_files()
    n = 0
    for f in xamls + cpps:
        raw = open(f, "rb").read()
        bom = raw.startswith(b"\xef\xbb\xbf")
        s = raw.decode("utf-8-sig")
        s2 = s.replace('"%s"' % old, '"%s"' % new).replace(">%s<" % old, ">%s<" % new)
        if s2 != s:
            n += 1
            open(f, "wb").write((b"\xef\xbb\xbf" if bom else b"") + s2.encode("utf-8"))
    print("renamed (%d source files changed)" % n)
    return 0


def cmd_lint(a):
    master, _ = read_lang(os.path.join(LANG_DIR, SOURCE + ".tsv"))
    allow = set()
    ap = os.path.join(ROOT, "tools", "i18n_allow.txt")
    if os.path.exists(ap):
        allow = {l.rstrip("\r\n") for l in open(ap, encoding="utf-8-sig") if l.strip() and not l.startswith("#")}
    n = 0
    xamls, cpps = source_files()
    for f in xamls:
        for attr, v in xaml_values(f):
            if attr in XAML_TEXT_ATTRS and v and not v.startswith("{") and v not in master and v not in allow and re.search(r"[A-Za-z\u4e00-\u9fff]", v):
                print("XAML %s: %s=\"%s\" is not an id" % (os.path.basename(f), attr, v[:60])); n += 1
    for f in cpps:
        if os.path.basename(f) in ("i18n.cpp", "i18n.h"):
            continue
        for ln, text, _, call in cpp_literals(f):
            if re.search(r"[\u4e00-\u9fff]", text) and text not in allow:
                print("%s:%d: Chinese literal not in the catalog: %s" % (os.path.basename(f), ln, text[:50])); n += 1
    print("%d findings" % n)
    return 1 if n else 0


# ---------------------------------------------------------------- review of the English texts by a person who knows the language

REVIEWED = os.path.join(ROOT, "tools", "en_reviewed.tsv")      # id TAB hash of the English text that was reviewed
REVIEW_TABLE = os.path.join(ROOT, "docs", "EN-REVIEW.md")


def read_reviewed():
    out = {}
    if os.path.exists(REVIEWED):
        for line in open(REVIEWED, encoding="utf-8-sig").read().split("\n"):
            line = line.rstrip("\r")
            if "\t" in line and not line.startswith("#"):
                k, h = line.split("\t")[:2]
                out[k] = h
    return out


def master_sections():
    """[(section, id)] of en-US.tsv in file order."""
    _, mlines = read_lang(os.path.join(LANG_DIR, SOURCE + ".tsv"))
    out, section = [], "other"
    for l in mlines:
        l = l.rstrip("\r")
        if l.startswith("## "):
            section = l[3:].strip()
        elif l and not l.startswith("#") and "\t" in l:
            out.append((section, l.split("\t")[0]))
    return out


def review_status(master):
    """{id: 'ok' | 'changed' | 'new'} against the hashes that were accepted."""
    rev = read_reviewed()
    st = {}
    for k, (t, _) in master.items():
        h = rev.get(k)
        st[k] = "new" if h is None else ("ok" if h == text_hash(t) else "changed")
    return st


def cmd_en_table(a):
    """Writes docs/EN-REVIEW.md: all English texts with their notes, grouped by area, with the state of the review."""
    master, _ = read_lang(os.path.join(LANG_DIR, SOURCE + ".tsv"))
    st = review_status(master)
    mlines = {}
    for l in open(os.path.join(LANG_DIR, SOURCE + ".tsv"), encoding="utf-8-sig").read().split("\n"):
        parts = l.rstrip("\r").split("\t")
        if len(parts) >= 3 and parts[0] and not parts[0].startswith("#"):
            mlines[parts[0]] = unesc(parts[2])
    counts = {"ok": 0, "changed": 0, "new": 0}
    for v in st.values():
        counts[v] += 1
    out = ["# English texts to review", "",
           "For a native speaker of English. Every text of the program is in **app/Assets/Lang/en-US.tsv** (the first language of the program: the",
           "other languages are translations of it). Please look at: wording that is not natural, not the way Windows says it, too long for a",
           "button, mistakes in spelling and punctuation, inconsistent terms (see I18N-STYLE.md). `{0}` `{1}` are filled in by the program",
           "(a name, a number); `{0:file|files}` chooses the word by the number.", "",
           "How to answer: send the ids with the better text (a list `id: better text` is enough). When the texts were looked at, the maintainer",
           "runs `python tools/i18n.py --en-accept --all`; a text that is changed later shows as **changed** here, so only that has to be looked at again.", "",
           "State: %d reviewed, %d changed since the review, %d not reviewed yet. (Generated by `python tools/i18n.py --en-table`; do not edit.)" % (counts["ok"], counts["changed"], counts["new"]), ""]
    section = None
    for sec, k in master_sections():
        if k not in master:
            continue
        if sec != section:
            section = sec
            out += ["", "## " + sec, "", "| id | English | note | state |", "| --- | --- | --- | --- |"]
        text = master[k][0].replace("|", "\\|").replace("\n", " / ")
        note = mlines.get(k, "").replace("|", "\\|")
        out.append("| `%s` | %s | %s | %s |" % (k, text, note, {"ok": "reviewed", "changed": "**changed**", "new": "not yet"}[st[k]]))
    open(REVIEW_TABLE, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
    print("docs/EN-REVIEW.md: %d texts (%d reviewed, %d changed, %d not yet)" % (len(master), counts["ok"], counts["changed"], counts["new"]))
    return 0


def cmd_en_accept(a):
    """Records that the current English text of the ids (or of all: --all) was reviewed."""
    master, _ = read_lang(os.path.join(LANG_DIR, SOURCE + ".tsv"))
    rev = read_reviewed()
    ids = list(master) if a.all else (a.en_accept or [])
    for k in ids:
        if k not in master:
            print("unknown id: " + k)
            return 1
        rev[k] = text_hash(master[k][0])
    rev = {k: v for k, v in rev.items() if k in master}
    lines = ["# id TAB hash of the English text that a reader looked at (tools/i18n.py --en-accept; --en-table writes docs/EN-REVIEW.md)"]
    lines += ["%s\t%s" % (k, rev[k]) for k in master if k in rev]
    open(REVIEWED, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print("accepted %d ids (%d reviewed in all)" % (len(ids), len(rev)))
    return cmd_en_table(a)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--review", action="store_true")
    ap.add_argument("--accept", nargs="*")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--rename", nargs=2, metavar=("OLD", "NEW"))
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--lint", action="store_true")
    ap.add_argument("--en-table", action="store_true")
    ap.add_argument("--en-accept", nargs="*")
    ap.add_argument("--lang")
    ap.add_argument("--allow-missing", action="store_true")
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if a.list:
        for k in read_lang(os.path.join(LANG_DIR, SOURCE + ".tsv"))[0]:
            print(k)
        return 0
    if a.en_table:
        return cmd_en_table(a)
    if a.en_accept is not None:
        return cmd_en_accept(a)
    if a.rename:
        return cmd_rename(a)
    if a.accept is not None or a.all:
        return cmd_accept(a)
    if a.review:
        return cmd_review(a)
    if a.update:
        return cmd_update(a)
    if a.lint:
        return cmd_lint(a)
    return cmd_check(a)


if __name__ == "__main__":
    sys.exit(main())
