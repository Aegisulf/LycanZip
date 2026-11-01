#!/usr/bin/env python3
"""Review tables of the translations, for people who check a language (a Markdown file they can fill in, and read back).

  python tools/i18n_review.py export ja-JP [--out DIR]   write DIR/LycanZip-ja-JP-review.md (default DIR: docs/review)
  python tools/i18n_review.py import ja-JP FILE.md       take the column "Suggested translation" of a filled-in table into
                                                         app/Assets/Lang/ja-JP.tsv (empty cells change nothing), then i18n.py --update;
                                                         --skip ID,ID leaves out suggestions that are not taken

The file has a "Read me" part (what to check, the style of the language, the glossary of tools/i18n_glossary.tsv) and a table "Strings"
(every text: number, id, where it is shown, the note for translators, English, the current translation, and empty columns for the
reviewer). Line breaks are shown as \\n and must stay; {0} {1} are filled in by the program; {0:one|other} chooses by the number.
In the table a | is written \\|, and a text that starts or ends with a space is put between backticks (`  `) so the spaces stay.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG_DIR = os.path.join(ROOT, "app", "Assets", "Lang")
GLOSSARY = os.path.join(ROOT, "tools", "i18n_glossary.tsv")

AREAS = {
    "main": "Main window, settings, dialogs",
    "welcome": "Welcome window (first start)",
    "task": "Task cards (progress and results)",
    "preview": "Picture preview, preview pane",
    "audio": "Audio preview",
    "scan": "Malware scan",
    "assoc": "File types (names shown in File Explorer)",
    "app": "Notifications, taskbar",
    "win11": "Windows 11 context menu",
    "drag": "Dragging items",
    "preset": "Built-in templates",
    "lang": "Language names (never translated)",
    "fmt": "Number, list and separator formats",
    "sums": "Checksum files",
    "core": "Error and warning messages",
    "shell": "File Explorer context menu",
}

STYLE = {
    "ja-JP": "Polite form (です / ます) in sentences; plain verbs on buttons and menu items (〜を開く). Windows 11 terms. Half-width "
             "space between Japanese and Latin text where Windows does (e.g. LycanZip を開く). No plural forms: {0} only.",
    "ko-KR": "Formal style (합니다 / 하세요) in sentences, nouns on buttons and menu items. Windows 11 terms (e.g. 압축 풀기, "
             "파일 탐색기). No plural forms: {0} only.",
    "de-DE": "Formal address (Sie). Windows 11 terms (Datei-Explorer, Kennwort, Standard-Apps). Keys as on German keyboards "
             "(Strg, Umschalt, Entf, Eingabe). German quotation marks „…“.",
    "es-ES": "Informal address (tú), as Windows 11 in Spanish (Spain). Windows terms (Explorador de archivos, Configuración). "
             "«…» quotation marks, decimal comma.",
    "fr-FR": "Formal address (vous). Windows 11 terms (Explorateur de fichiers, Paramètres). « … » quotation marks with spaces, a "
             "space before : ; ? ! , decimal comma, Ko / Mo / Go for sizes.",
}

HEADER = ["No.", "ID", "Where", "Note", "English", "{lang}", "Suggested translation", "Comment"]


def read_rows(path):
    """id -> (text, third column), plus the ids in file order."""
    entries, order = {}, []
    for line in open(path, encoding="utf-8-sig").read().split("\n"):
        line = line.rstrip("\r")
        if not line or line.startswith("#") or "\t" not in line:
            continue
        parts = line.split("\t")
        entries[parts[0]] = (parts[1] if len(parts) > 1 else "", parts[2] if len(parts) > 2 else "")
        order.append(parts[0])
    return entries, order


def glossary_for(lang):
    rows, header = [], None
    for line in open(GLOSSARY, encoding="utf-8").read().splitlines():
        if not line or line.startswith("#"):
            continue
        cells = line.split("\t")
        if header is None:
            header = cells
            continue
        if lang in header:
            i = header.index(lang)
            rows.append((cells[0], cells[i] if i < len(cells) else ""))
    return rows


def cell(text):
    """A table cell: | escaped, and spaces at the ends kept by backticks (a cell is trimmed otherwise)."""
    text = str(text).replace("|", "\\|")
    if text and text != text.strip():
        return "`%s`" % text
    return text


def uncell(text):
    text = text.strip()
    if len(text) >= 2 and text.startswith("`") and text.endswith("`"):
        text = text[1:-1]
    return text.replace("\\|", "|")


def md_table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(cell(c) for c in r) + " |" for r in rows]
    return out


def parse_md_table(text, title):
    """The rows of the first table after the heading `title`, as dicts by column name."""
    lines = text.replace("\r\n", "\n").split("\n")
    start = next(i for i, l in enumerate(lines) if l.strip().lstrip("#").strip() == title)
    table = []
    for l in lines[start + 1:]:
        if l.startswith("|"):
            table.append([c for c in re.split(r"(?<!\\)\|", l)[1:-1]])
        elif table:
            break
    header = [c.strip() for c in table[0]]
    return [dict(zip(header, (uncell(c) for c in r))) for r in table[2:]]


def cmd_export(lang, out_dir):
    master, order = read_rows(os.path.join(LANG_DIR, "en-US.tsv"))
    tr, _ = read_rows(os.path.join(LANG_DIR, lang + ".tsv"))
    md = ["# LycanZip: review of the %s translation" % lang, "",
          "## Read me", "",
          "LycanZip is a free archive manager for Windows (ZIP, 7z, RAR ...). This file lists every text of its interface in English "
          "and in %s. The %s texts were translated by machine and need a check by someone who speaks the language." % (lang, lang), "",
          "### How to review", "",
          "1. Go to the table \"Strings\" below. Each row is one text; the column \"Where\" says where it is shown, \"Note\" explains it.",
          "2. If a translation is wrong, unnatural or too long, write the better text into the column \"Suggested translation\". Leave "
          "the cell empty when the translation is fine. Use \"Comment\" for questions.",
          "3. Keep {0}, {1} ... exactly (the program puts a number, a name or a size there). {0:one|other} chooses the first form when "
          "the number is 1 and the second otherwise (for example {0} {0:file|files}); languages without plural forms just write {0}. "
          "Keep \\n (a line break).",
          "4. Inside the table write a | as \\|. A text that starts or ends with a space is between backticks (`` ` ``): keep the "
          "backticks and the spaces in your suggestion.",
          "5. Button and menu texts should stay short. File names, LycanZip, ZIP / 7z / RAR and Windows feature names that Windows "
          "itself does not translate stay as they are.",
          "6. Send the file back. The suggestions are read back automatically, so please do not change the other columns or the "
          "order of the columns.", "",
          "### Style for %s" % lang, "",
          STYLE.get(lang, "Use the terms of Windows 11 in this language."), "",
          "### Glossary", "",
          "Please tell us if a word should be different: it is used everywhere.", ""]
    md += md_table(["English", lang], glossary_for(lang))
    md += ["", "## Strings", ""]
    rows = []
    for n, k in enumerate(order, 1):
        en, note = master[k]
        rows.append([n, k, AREAS.get(k.split(".")[0], k.split(".")[0]), note, en, tr.get(k, ("", ""))[0], "", ""])
    md += md_table([h.format(lang=lang) for h in HEADER], rows)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "LycanZip-%s-review.md" % lang)
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(md) + "\n")
    print("%s: %d texts -> %s" % (lang, len(rows), os.path.relpath(path, ROOT)))
    return 0


def cmd_import(lang, path, skip=()):
    master, _ = read_rows(os.path.join(LANG_DIR, "en-US.tsv"))
    changes = {}
    for r in parse_md_table(open(path, encoding="utf-8-sig").read(), "Strings"):
        k, new = r.get("ID", ""), r.get("Suggested translation", "")
        if not k or k in skip:
            continue
        # a cell of only spaces is a suggestion when the English text is only spaces too (separators such as fmt.gap)
        if new.strip() or (new and not master.get(k, ("x", ""))[0].strip()):
            changes[k] = new.replace("\t", "\\t")
    if not changes:
        print("no suggestions in %s" % path)
        return 0
    target = os.path.join(LANG_DIR, lang + ".tsv")
    raw = open(target, "rb").read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    done = set()
    for i, line in enumerate(lines):
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if parts[0] in changes:
            parts[1] = changes[parts[0]]
            lines[i] = "\t".join(parts[:2])   # no hash: --update records the one of the current English text
            done.add(parts[0])
    open(target, "wb").write((b"\xef\xbb\xbf" if bom else b"") + nl.join(lines).encode("utf-8"))
    unknown = sorted(set(changes) - done)
    print("%s: %d suggestions applied%s" % (lang, len(done), (", unknown ids: " + ", ".join(unknown)) if unknown else ""))
    subprocess.call([sys.executable, os.path.join(ROOT, "tools", "i18n.py"), "--update", "--lang", lang])
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["export", "import"])
    ap.add_argument("lang")
    ap.add_argument("file", nargs="?")
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "review"))
    ap.add_argument("--skip", default="", help="import: ids whose suggestion is not taken, separated by commas")
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if a.command == "export":
        return cmd_export(a.lang, a.out)
    if not a.file:
        print("import needs the file")
        return 2
    return cmd_import(a.lang, a.file, {s.strip() for s in a.skip.split(",") if s.strip()})


if __name__ == "__main__":
    sys.exit(main())
