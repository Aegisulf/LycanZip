"""End-to-end test: makes test archives with 7z.exe, then lists / extracts / cats / tests them with lycanzip and compares the content.

Usage: python cli_roundtrip.py <path-to-lycanzip.exe>
Needs 7z.exe on PATH (a Scoop shim is fine). The zip slip malicious files are made by hand with the Python zipfile.
"""
import hashlib
import os
import shutil
import subprocess

# None of the child processes inherits stdin: for an archive with encrypted headers 7z waits for a password on stdin, which would hang for ever when the test is run from an interactive shell
_subprocess_run = subprocess.run


def _run_without_stdin(*args, **kwargs):
    kwargs.setdefault("stdin", subprocess.DEVNULL)
    return _subprocess_run(*args, **kwargs)


subprocess.run = _run_without_stdin
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

exe = sys.argv[1]
failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", msg)
    else:
        print("ok  :", msg)


def run(*args, check_rc=None):
    p = subprocess.run([exe, *args], capture_output=True)
    if check_rc is not None:
        check(p.returncode == check_rc, f"rc={p.returncode} (want {check_rc}) for {args[:2]}")
    return p


def make_legacy_zip(path, entries):
    """Writes a ZIP by hand: the names are raw bytes (code page encoded), no UTF-8 flag, no Unicode extra field,
    imitating the old-style archives made by Chinese / Japanese / Korean Windows archivers. entries: [(name_bytes, data_bytes)]"""
    import struct, zlib
    out = bytearray()
    central = bytearray()
    for name, data in entries:
        crc = zlib.crc32(data) & 0xFFFFFFFF
        off = len(out)
        out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, 0, 0, 0x21, crc, len(data), len(data), len(name), 0)
        out += name + data
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, 0, 0, 0x21, crc, len(data), len(data),
                               len(name), 0, 0, 0, 0, 0, off)
        central += name
    cd_off = len(out)
    out += central
    out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(entries), len(entries), len(central), cd_off, 0)
    Path(path).write_bytes(bytes(out))


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


root = Path(tempfile.mkdtemp(prefix="lycan_test_"))
try:
    src = root / "src"
    (src / "sub").mkdir(parents=True)
    (src / "a.txt").write_text("hello lycan\n" * 100, encoding="utf-8")
    (src / "sub" / "b.bin").write_bytes(os.urandom(50_000))
    (src / "中文檔名.txt").write_text("測試", encoding="utf-8")

    for ext, extra in [("zip", []), ("7z", []), ("7z", ["-ms=on"]), ("tar", []), ("zip", ["-pSecret", "-mem=AES256"])]:
        name = f"t{len(extra)}.{ext}" if not extra else "t_" + "_".join(a.strip("-=") for a in extra) + f".{ext}"
        arc = root / name
        if arc.exists():
            arc.unlink()
        r = subprocess.run(["7z", "a", str(arc), ".\\*", *extra], cwd=src, capture_output=True)
        if r.returncode != 0:
            print("7z failed:", r.stderr.decode(errors="replace"))
            failures += 1
            continue
        pw = ["-p", "Secret"] if "-pSecret" in extra else []

        lst = run("l", str(arc), *pw, check_rc=0)
        check(b"a.txt" in lst.stdout and "中文檔名.txt".encode() in lst.stdout, f"{name}: list shows names")

        out = root / ("out_" + name)
        run("x", str(arc), "-o", str(out), *pw, check_rc=0)
        check((out / "a.txt").exists() and sha(out / "a.txt") == sha(src / "a.txt"), f"{name}: a.txt roundtrip")
        check(sha(out / "sub" / "b.bin") == sha(src / "sub" / "b.bin"), f"{name}: b.bin roundtrip")
        check((out / "中文檔名.txt").read_text(encoding="utf-8") == "測試", f"{name}: unicode filename")

        run("t", str(arc), *pw, check_rc=0)

    # cat: find the index of a.txt, verify the streamed content
    arc = root / "t0.zip"
    lst = run("l", str(arc)).stdout.decode("utf-8", errors="replace").splitlines()
    idx = next(l.split("\t")[0].strip() for l in lst if l.endswith("a.txt"))
    cat = run("cat", str(arc), idx, check_rc=0)
    check(cat.stdout == (src / "a.txt").read_bytes(), "cat streams entry content")

    # wrong password / none given
    enc = next(root.glob("t_*AES*.zip"), None)
    if enc:
        p = run("x", str(enc), "-o", str(root / "nopw"))
        check(p.returncode != 0, "encrypted without password fails")
        p = run("x", str(enc), "-o", str(root / "badpw"), "-p", "wrong")
        check(p.returncode != 0, "encrypted with wrong password fails")
        check(not (root / "badpw" / "a.txt").exists() or sha(root / "badpw" / "a.txt") != sha(src / "a.txt"),
              "wrong password leaves no valid file")

    # Zip Slip
    evil = root / "evil.zip"
    with zipfile.ZipFile(evil, "w") as z:
        z.writestr("ok.txt", "fine")
        z.writestr("../escaped.txt", "bad")
        z.writestr("..\\escaped2.txt", "bad")
        z.writestr("/abs.txt", "bad")
        z.writestr("CON.txt", "reserved")
    dest = root / "evil_out"
    p = run("x", str(evil), "-o", str(dest))
    text = p.stdout.decode("utf-8", errors="replace")
    check((dest / "ok.txt").exists(), "evil.zip: normal file extracted")
    check(not (root / "escaped.txt").exists() and not (root / "escaped2.txt").exists(), "evil.zip: no traversal")
    check(not any(dest.parent.glob("abs.txt")) and not Path("/abs.txt").exists(), "evil.zip: no absolute write")
    check("Rejected" in text, "evil.zip: rejections reported")
    check((dest / "_CON.txt").exists(), "evil.zip: reserved name renamed")

    # overwrite behaviour
    p = run("x", str(evil), "-o", str(dest))
    check(p.returncode != 0, "existing files without -y/-s -> error")
    run("x", str(evil), "-o", str(dest), "-y", check_rc=None)
    p = run("x", str(evil), "-o", str(dest), "-s")
    check(b"ok.txt" in p.stdout, "-s skips existing")

    # double-layer formats: tar.gz / tgz / tar.xz / tar.bz2
    base_tar = root / "inner.tar"
    subprocess.run(["7z", "a", str(base_tar), ".\\*"], cwd=src, capture_output=True, check=True)
    for name, fmt in [("dbl.tar.gz", "gzip"), ("dbl.tgz", "gzip"), ("dbl.tar.xz", "xz"), ("dbl.tar.bz2", "bzip2")]:
        arc = root / name
        subprocess.run(["7z", "a", f"-t{fmt}", str(arc), str(base_tar)], capture_output=True, check=True)
        lst = run("l", str(arc), check_rc=0).stdout.decode("utf-8", errors="replace")
        check("tar (" in lst and "a.txt" in lst, f"{name}: listed as tar inside {fmt}")
        out = root / ("out_" + name)
        run("x", str(arc), "-o", str(out), check_rc=0)
        check((out / "a.txt").exists() and sha(out / "a.txt") == sha(src / "a.txt"), f"{name}: extracted tar content")
        check(sha(out / "sub" / "b.bin") == sha(src / "sub" / "b.bin"), f"{name}: nested file roundtrip")
    leftovers = list(Path(tempfile.gettempdir()).glob("lycanzip_*.tmp"))
    check(not leftovers, "no temp files left behind")

    # a plain .gz (not tar) keeps the single-file view
    plain = root / "note.txt"
    plain.write_text("plain", encoding="utf-8")
    subprocess.run(["7z", "a", "-tgzip", str(root / "note.txt.gz"), str(plain)], capture_output=True, check=True)
    lst = run("l", str(root / "note.txt.gz"), check_rc=0).stdout.decode("utf-8", errors="replace")
    check("tar (" not in lst and "note.txt" in lst, "plain .gz stays a single-file archive")

    # split volumes: open .7z.001
    big = root / "bigsrc"
    big.mkdir()
    (big / "data.bin").write_bytes(os.urandom(200_000))
    vol = root / "vol.7z"
    subprocess.run(["7z", "a", "-v50k", "-mx=0", str(vol), str(big / "data.bin")], capture_output=True, check=True)
    check((root / "vol.7z.002").exists(), "test setup: multi-volume created")
    out = root / "out_vol"
    run("x", str(root / "vol.7z.001"), "-o", str(out), check_rc=0)
    check((out / "data.bin").exists() and sha(out / "data.bin") == sha(big / "data.bin"), "multi-volume .7z.001 extracts")

    # smart extraction
    one = root / "one_folder.zip"
    subprocess.run(["7z", "a", str(one), "wrap"], cwd=root / "smart_src", capture_output=True) if (root / "smart_src").exists() else None
    ss = root / "smart_src"
    (ss / "wrap" / "inner").mkdir(parents=True)
    (ss / "wrap" / "inner" / "f.txt").write_text("x")
    (ss / "loose1.txt").write_text("1")
    (ss / "loose2.txt").write_text("2")
    subprocess.run(["7z", "a", str(one), "wrap"], cwd=ss, capture_output=True, check=True)
    multi = root / "multi_top.zip"
    subprocess.run(["7z", "a", str(multi), "loose1.txt", "loose2.txt"], cwd=ss, capture_output=True, check=True)
    single = root / "single_file.zip"
    subprocess.run(["7z", "a", str(single), "loose1.txt"], cwd=ss, capture_output=True, check=True)

    for arcname, expect in [("one_folder.zip", "wrap/inner/f.txt"), ("multi_top.zip", "multi_top/loose1.txt"),
                            ("single_file.zip", "loose1.txt")]:
        d = root / ("smart_" + arcname.replace(".zip", ""))
        d.mkdir()
        shutil.copy(root / arcname, d / arcname)
        p = run("x", str(d / arcname))
        check((d / expect).exists(), f"smart extract {arcname} -> {expect}")
        check(p.returncode == 0, f"smart extract {arcname} rc=0")

    # File name encoding: make a zip with 7z in an old code page (no UTF-8 flag), verify the automatic detection and the manual choice
    enc_cases = [
        ("big5", "950", ["測試資料夾/報告書.docx", "測試資料夾/會議紀錄.txt", "使用手冊.pdf"]),
        ("gbk", "936", ["测试资料夹/报告书.docx", "测试资料夹/会议记录.txt", "使用手册.pdf"]),
        ("sjis", "932", ["テスト/日本語のファイル.txt", "設計書/説明資料.pptx", "ひらがなカタカナ.txt"]),
        ("kr", "949", ["한국어/파일.txt", "프로젝트/설계문서.docx", "안녕하세요.txt"]),
    ]
    pycp = {"950": "cp950", "936": "gbk", "932": "cp932", "949": "cp949"}
    for tag, cp, names in enc_cases:
        arc = root / f"enc_{tag}.zip"
        make_legacy_zip(arc, [(n.encode(pycp[cp]), ("data " + n).encode("utf-8")) for n in names])
        lst = run("l", str(arc), check_rc=0).stdout.decode("utf-8", errors="replace")
        check(all(n.split("/")[-1] in lst for n in names), f"{tag}: names auto-detected correctly")
        out = root / f"out_enc_{tag}"
        run("x", str(arc), "-o", str(out), check_rc=0)
        check(all((out / n).exists() for n in names), f"{tag}: extracted to correct on-disk names")
    # forcing a wrong encoding should change the result; the right encoding should fix it
    arc = root / "enc_big5.zip"
    wrong = run("l", str(arc), "--encoding", "sjis", check_rc=0).stdout.decode("utf-8", errors="replace")
    check("使用手冊.pdf" not in wrong, "forcing a wrong encoding changes the names")
    right = run("l", str(arc), "--encoding", "big5", check_rc=0).stdout.decode("utf-8", errors="replace")
    check("使用手冊.pdf" in right and "[given]" in right, "forcing the right encoding works")
    # a zip with the UTF-8 flag is not affected
    uarc = root / "enc_utf8flag.zip"
    with zipfile.ZipFile(uarc, "w") as zf:  # Python sets the UTF-8 flag automatically for non-ASCII names
        zf.writestr("使用手冊.pdf", "x")
        zf.writestr("テスト.txt", "x")
    ul = run("l", str(uarc), check_rc=0).stdout.decode("utf-8", errors="replace")
    check("使用手冊.pdf" in ul and "テスト.txt" in ul, "UTF-8 flagged zip keeps correct names")
    check("[detected automatically" not in ul, "UTF-8 flagged zip is not treated as legacy-encoded")
    # with the flag, also the names whose characters all exist in CP437 stay as they are (Python alone would write these without the flag)
    class Utf8Name(zipfile.ZipInfo):
        def _encodeFilenameFlags(self):
            return self.filename.encode("utf-8"), 0x800
    latin = ["Café à Paris.jpg", "München im Winter.jpg", "España.jpg", "使用手冊.pdf"]
    larc = root / "enc_utf8flag_latin.zip"
    with zipfile.ZipFile(larc, "w") as zf:
        for n in latin:
            zf.writestr(Utf8Name(n), "x")
    ll = run("l", str(larc), check_rc=0).stdout.decode("utf-8", errors="replace")
    check(all(n in ll for n in latin), "UTF-8 flagged zip keeps names that fit CP437")
    check("[detected automatically" not in ll, "UTF-8 flagged names that fit CP437 are not treated as legacy-encoded")
    lout = root / "out_utf8flag_latin"
    run("x", str(larc), "-o", str(lout), check_rc=0)
    check(all((lout / n).exists() for n in latin), "UTF-8 flagged names that fit CP437 are extracted correctly")

    # ---- Creating archives (a): verify the products with the independent 7z.exe ----
    def tree_hash(d):
        h = {}
        for p in sorted(Path(d).rglob("*")):
            if p.is_file():
                h[p.relative_to(d).as_posix()] = sha(p)
        return h

    csrc = root / "csrc"
    (csrc / "docs" / "sub").mkdir(parents=True)
    (csrc / "docs" / "a.txt").write_text("hello\n" * 500, encoding="utf-8")
    (csrc / "docs" / "sub" / "b.bin").write_bytes(os.urandom(120_000))
    (csrc / "docs" / "中文檔名.txt").write_text("測試", encoding="utf-8")
    (csrc / "docs" / "empty").mkdir()
    (csrc / "single.txt").write_text("single", encoding="utf-8")
    expect_docs = tree_hash(csrc / "docs")

    for name in ["c.zip", "c.7z", "c.tar", "c.tar.gz", "c.tgz", "c.tar.xz", "c.tar.bz2"]:
        arc = root / name
        run("a", str(arc), str(csrc / "docs"), str(csrc / "single.txt"), check_rc=0)
        check(arc.exists() and not Path(str(arc) + ".lycantmp").exists(), f"create {name}: output exists, no temp left")
        # independent check 1: 7z.exe can test and unpack it
        ext = root / ("x7_" + name)
        r = subprocess.run(["7z", "x", f"-o{ext}", "-y", str(arc)], capture_output=True)
        if name.endswith((".gz", ".tgz", ".xz", ".bz2")):  # 7z unpacks only the outer layer, giving a .tar
            inner = next(ext.glob("*.tar"), None)
            check(inner is not None, f"create {name}: 7z.exe unwraps outer layer")
            if inner:
                ext2 = root / ("x7b_" + name)
                subprocess.run(["7z", "x", f"-o{ext2}", "-y", str(inner)], capture_output=True)
                ext = ext2
        check(tree_hash(ext / "docs") == expect_docs, f"create {name}: 7z.exe extracts identical docs/ tree")
        check((ext / "single.txt").read_text(encoding="utf-8") == "single", f"create {name}: second input at top level")
        check((ext / "docs" / "empty").is_dir(), f"create {name}: empty folder preserved")
        # independent check 2: our own x
        out = root / ("xl_" + name)
        run("x", str(arc), "-o", str(out), check_rc=0)
        check(tree_hash(out / "docs") == expect_docs, f"create {name}: lycanzip x roundtrip")

    # password
    for name, extra, label in [("e_aes.zip", [], "zip AES-256"), ("e_compat.zip", ["--profile", "compatible"], "zip ZipCrypto"),
                               ("e_7z.7z", [], "7z"), ("e_7zhdr.7z", ["--encrypt-names"], "7z + encrypted names")]:
        arc = root / name
        p = run("a", str(arc), str(csrc / "docs"), "-p", "S3cret", *extra, check_rc=0)
        check(not (name == "e_compat.zip") or "ZipCrypto" in p.stdout.decode("utf-8", errors="replace"), f"{label}: weak-crypto warning shown")
        r = subprocess.run(["7z", "t", "-pS3cret", str(arc)], capture_output=True)
        check(r.returncode == 0, f"{label}: 7z.exe validates with password")
        r = subprocess.run(["7z", "t", "-pWRONG", str(arc)], capture_output=True)
        check(r.returncode != 0, f"{label}: wrong password rejected by 7z.exe")
        out = root / ("xe_" + name)
        run("x", str(arc), "-o", str(out), "-p", "S3cret", check_rc=0)
        check(tree_hash(out / "docs") == expect_docs, f"{label}: lycanzip x with password")
        if name == "e_7zhdr.7z":
            r = subprocess.run(["7z", "l", str(arc)], capture_output=True)
            check(b"a.txt" not in r.stdout, f"{label}: file names hidden without password")

    # split volumes: 7z / zip
    bigdir = root / "bigsrc2"
    bigdir.mkdir()
    (bigdir / "data.bin").write_bytes(os.urandom(300_000))
    for name in ["v.7z", "v.zip"]:
        arc = root / name
        run("a", str(arc), str(bigdir), "-v100k", "-l", "0", check_rc=0)
        vols = sorted(root.glob(name + ".0*"))
        check(len(vols) >= 3 and all(v.stat().st_size == 100 * 1024 for v in vols[:-1]), f"{name}: split into fixed-size volumes ({len(vols)})")
        check(not arc.exists(), f"{name}: no unsplit file left")
        ext = root / ("xv7_" + name)
        subprocess.run(["7z", "x", f"-o{ext}", "-y", str(vols[0])], capture_output=True)
        check((ext / "bigsrc2" / "data.bin").exists() and sha(ext / "bigsrc2" / "data.bin") == sha(bigdir / "data.bin"), f"{name}: 7z.exe extracts volumes")
        out = root / ("xvl_" + name)
        run("x", str(vols[0]), "-o", str(out), check_rc=0)
        check((out / "bigsrc2" / "data.bin").exists() and sha(out / "bigsrc2" / "data.bin") == sha(bigdir / "data.bin"), f"{name}: lycanzip extracts volumes")

    # overwrite protection, the output file is not put into itself, an input that does not exist
    p = run("a", str(root / "c.zip"), str(csrc / "single.txt"))
    check(p.returncode != 0, "create: refuses to overwrite existing archive")
    run("a", str(root / "c.zip"), str(csrc / "single.txt"), "-y", check_rc=0)
    inside = csrc / "docs" / "self.zip"
    p = run("a", str(inside), str(csrc / "docs"), check_rc=0)
    lst = subprocess.run(["7z", "l", "-ba", str(inside)], capture_output=True).stdout  # -ba: do not print the path of the archive itself
    check(b"a.txt" in lst and b"self.zip" not in lst, "create: output archive is not packed into itself")
    # overwrite the same archive that is inside the input again: the old file is in the folder, but must not be put into the new one
    run("a", str(inside), str(csrc / "docs"), "-y", check_rc=0)
    lst = subprocess.run(["7z", "l", "-ba", str(inside)], capture_output=True).stdout
    check(b"self.zip" not in lst, "create: existing output inside input is excluded on overwrite")
    p = run("a", str(root / "nx.zip"), str(root / "does_not_exist"))
    check(p.returncode != 0 and not (root / "nx.zip").exists(), "create: missing input fails without leaving output")

    # a junction is not followed
    junc = csrc / "docs" / "linkdir"
    r = subprocess.run(["cmd", "/c", "mklink", "/J", str(junc), str(csrc / "docs" / "sub")], capture_output=True)
    if r.returncode == 0:
        p = run("a", str(root / "junc.zip"), str(csrc / "docs"), "-y", check_rc=0)
        lst = subprocess.run(["7z", "l", str(root / "junc.zip")], capture_output=True).stdout
        check(b"linkdir" not in lst and "Skipped".encode() in p.stdout, "create: junction skipped, not followed")
        os.rmdir(junc)

    # an unknown extension needs -t
    p = run("a", str(root / "weird.xyz"), str(csrc / "single.txt"))
    check(p.returncode == 2, "create: unknown extension requires -t")
    run("a", str(root / "weird.xyz"), str(csrc / "single.txt"), "-t", "zip", check_rc=0)

    # regression test: 7z (data level encryption) extracted with a wrong password must report failure, not a false success
    wp = root / "wp.7z"
    run("a", str(wp), str(csrc / "docs"), "-p", "S3cret", check_rc=0)
    p = run("x", str(wp), "-o", str(root / "wp_out"), "-p", "wrong")
    check(p.returncode != 0, "7z extract with wrong password reports failure (not silent success)")
    check(not (root / "wp_out" / "docs" / "a.txt").exists(), "7z wrong password leaves no extracted file")

    # -r renames automatically
    rn = root / "rename_out"
    run("x", str(root / "t0.zip"), "-o", str(rn), check_rc=0)
    run("x", str(root / "t0.zip"), "-o", str(rn), "-r", check_rc=0)
    check((rn / "a.txt").exists() and (rn / "a (2).txt").exists() and sha(rn / "a (2).txt") == sha(src / "a.txt"),
          "x -r renames conflicting files")

    # Relative paths: extracting into "." (and with no -o at all) from the archive's own folder must work
    rel = root / "relcwd"
    rel.mkdir()
    (rel / "only.txt").write_text("relative extraction")
    run("a", str(root / "rel.zip"), str(rel / "only.txt"), check_rc=0)
    work = root / "relwork"
    work.mkdir()
    shutil.copy(root / "rel.zip", work / "rel.zip")
    p = subprocess.run([exe, "x", "rel.zip"], cwd=work, capture_output=True)
    check(p.returncode == 0 and (work / "only.txt").read_text() == "relative extraction", "x with a relative archive path and no -o extracts into the current folder")
    (work / "only.txt").unlink()
    p = subprocess.run([exe, "x", "rel.zip", "-o", ".", "--no-smart"], cwd=work, capture_output=True)
    check(p.returncode == 0 and (work / "only.txt").exists(), "x -o . extracts into the current folder")
    p = subprocess.run([exe, "x", "rel.zip", "-o", "sub/deeper", "-y"], cwd=work, capture_output=True)
    check(p.returncode == 0 and (work / "sub" / "deeper" / "only.txt").exists(), "x -o with a relative sub folder works")

    # Decompression-bomb limits (--max-size / --max-ratio): refuse before writing anything
    bomb_src = root / "bomb_src"
    bomb_src.mkdir()
    (bomb_src / "zeros.bin").write_bytes(b"\0" * (32 << 20))
    bomb = root / "bomb.zip"
    run("a", str(bomb), str(bomb_src), check_rc=0)
    p = run("x", str(bomb), "-o", str(root / "bomb_o1"), "--max-size", "8m")
    check(p.returncode != 0 and not (root / "bomb_o1").exists(), "x --max-size refuses an archive that expands beyond the limit and writes nothing")
    p = run("x", str(bomb), "-o", str(root / "bomb_o2"), "--max-ratio", "50")
    check(p.returncode != 0 and not (root / "bomb_o2").exists(), "x --max-ratio refuses an archive with an absurd ratio and writes nothing")
    run("x", str(bomb), "-o", str(root / "bomb_o3"), "--max-size", "1g", "--max-ratio", "1000000", check_rc=0)
    check((root / "bomb_o3" / "bomb_src" / "zeros.bin").stat().st_size == 32 << 20, "x with limits that are not exceeded extracts normally")

    # u: change an existing archive (add / delete / rename / mkdir / replace), estimate, and what it refuses
    upd_src = root / "upd_src"
    (upd_src / "docs").mkdir(parents=True)
    (upd_src / "docs" / "a.txt").write_text("alpha " * 1000)
    (upd_src / "keep.txt").write_text("keep me")
    (root / "upd_new.txt").write_text("brand new")
    (root / "upd_edit.txt").write_text("edited")
    for ext in ("zip", "7z"):
        arc = root / ("upd." + ext)
        run("a", str(arc), str(upd_src / "docs"), str(upd_src / "keep.txt"), check_rc=0)
        p = run("u", str(arc), "--into", "docs", "--add", str(root / "upd_new.txt"), "--delete", "keep.txt",
                "--rename", "docs/a.txt", "docs/b.txt", "--mkdir", "fresh", "--replace", "docs/upd_new.txt", str(root / "upd_edit.txt"))
        check(p.returncode == 0, f"u ({ext}): add / delete / rename / mkdir / replace in one call")
        listing = run("l", str(arc), check_rc=0).stdout.decode("utf-8", errors="replace")
        check("docs/b.txt" in listing and "docs/a.txt" not in listing and "keep.txt" not in listing and "fresh" in listing,
              f"u ({ext}): the listing shows the changes")
        out = root / ("upd_out_" + ext)
        run("x", str(arc), "-o", str(out), "--no-smart", check_rc=0)
        check((out / "docs" / "b.txt").read_text() == "alpha " * 1000 and (out / "docs" / "upd_new.txt").read_text() == "edited" and (out / "fresh").is_dir(),
              f"u ({ext}): the content is right after extracting")
        before = arc.read_bytes()
        p = run("u", str(arc), "--delete", "no/such/item")
        check(p.returncode == 1 and arc.read_bytes() == before and not (root / (arc.name + ".lycantmp")).exists(),
              f"u ({ext}): a bad operation fails and leaves the archive alone")
        p = run("u", str(arc), "--estimate", "--delete", "docs/b.txt", check_rc=0)
        check(b"bytes" in p.stdout and arc.read_bytes() == before, f"u ({ext}): --estimate prints the cost and changes nothing")
    p = run("u", str(root / "upd.zip"), "--backup", "--delete", "fresh", check_rc=0)
    check((root / "upd.zip.bak").exists(), "u --backup keeps the old archive as .bak")
    tgz = root / "upd.tar.gz"
    run("a", str(tgz), str(upd_src / "keep.txt"), check_rc=0)
    p = run("u", str(tgz), "--delete", "keep.txt")
    check(p.returncode == 1, "u refuses a tar.gz (cannot be changed in place)")
    p = run("u", str(root / "upd.zip"))
    check(p.returncode == 2, "u without operations prints the usage")

    # info / --version: version and environment diagnostics
    ver = (Path(__file__).resolve().parents[1] / "VERSION").read_text().strip()
    p = run("--version", check_rc=None)
    check(p.returncode == 0 and ver.encode() in p.stdout, f"--version prints the VERSION file value ({ver})")
    info = subprocess.run([exe, "info"], capture_output=True).stdout.decode("utf-8", errors="replace")
    check(ver in info and "7z.dll:" in info and "Settings file:" in info and "Portable mode:" in info, "info prints version, 7z.dll, settings path and portable mode")

    # hash: the output of sha256sum / md5sum, several algorithms, an archive, checksum files
    hdir = root / "hashing"
    hdir.mkdir()
    (hdir / "a b.txt").write_text("hash me\n" * 100, encoding="utf-8")
    (hdir / "中文.bin").write_bytes(os.urandom(70_000))
    (hdir / "sub").mkdir()
    (hdir / "sub" / "c.txt").write_text("inner", encoding="utf-8")
    def want(path, algo):
        return hashlib.new(algo, Path(path).read_bytes()).hexdigest()
    p = run("hash", str(hdir / "a b.txt"), str(hdir / "中文.bin"), check_rc=0)
    out = p.stdout.decode("utf-8")
    check(out.splitlines() == [f"{want(hdir / 'a b.txt', 'sha256')} *{hdir / 'a b.txt'}", f"{want(hdir / '中文.bin', 'sha256')} *{hdir / '中文.bin'}"],
          "hash: SHA-256 by default, in the format of sha256sum")
    p = run("hash", str(hdir / "a b.txt"), "-a", "md5,sha1,sha512", check_rc=0)
    lines = p.stdout.decode("utf-8").splitlines()
    check(lines == [f"MD5 ({hdir / 'a b.txt'}) = {want(hdir / 'a b.txt', 'md5')}", f"SHA1 ({hdir / 'a b.txt'}) = {want(hdir / 'a b.txt', 'sha1')}",
                    f"SHA512 ({hdir / 'a b.txt'}) = {want(hdir / 'a b.txt', 'sha512')}"], "hash: several algorithms are written as 'ALGO (name) = value'")
    run("hash", str(hdir / "missing.bin"), check_rc=1)
    run("hash", str(hdir / "a b.txt"), "-a", "nope", check_rc=2)
    # written to a file, then checked (names relative to the checksum file)
    os.chdir(hdir)
    try:
        run("hash", "a b.txt", "中文.bin", "sub/c.txt", "-o", "SHA256SUMS", check_rc=0)
        sums = (hdir / "SHA256SUMS").read_bytes().decode("utf-8")
        check(f"{want(hdir / 'a b.txt', 'sha256')} *a b.txt" in sums and "*中文.bin" in sums, "hash -o: the checksum file is written (UTF-8)")
        p = run("hash", "--check", "SHA256SUMS", check_rc=0)
        check("All 3 files match" in p.stdout.decode("utf-8"), "hash --check: all three files match")
        (hdir / "a b.txt").write_text("changed", encoding="utf-8")
        (hdir / "中文.bin").unlink()
        p = run("hash", "--check", "SHA256SUMS", check_rc=1)
        o = p.stdout.decode("utf-8")
        check("a b.txt: FAILED" in o and "中文.bin: MISSING" in o and "sub/c.txt" not in o and "1 of 3 match" in o, "hash --check: a changed file fails, a missing one is reported, what matches is not listed")
        # a checksum file that sha256sum / md5sum would write (text mode, upper case) and a BSD one
        (hdir / "sums2.md5").write_text(f"{want(hdir / 'sub' / 'c.txt', 'md5').upper()}  sub/c.txt\n", encoding="utf-8")
        run("hash", "--check", "sums2.md5", check_rc=0)
        (hdir / "sums3.txt").write_text(f"SHA1 (sub/c.txt) = {want(hdir / 'sub' / 'c.txt', 'sha1')}\n", encoding="utf-8")
        run("hash", "--check", "sums3.txt", check_rc=0)
        # verify: several checksum files at once, .sfv, a checksum file that cannot be used
        (hdir / "ok.sfv").write_text("; made by a test\nsub/c.txt %08X\n" % (zlib.crc32((hdir / "sub" / "c.txt").read_bytes()) & 0xFFFFFFFF), encoding="utf-8")
        p = run("verify", "ok.sfv", "sums2.md5", check_rc=0)
        check(p.stdout.decode("utf-8").count("All 1 files match") == 2, "verify: two checksum files are both checked (.sfv and .md5)")
        (hdir / "empty.sha256").write_text("not a checksum file\n", encoding="utf-8")
        run("verify", "empty.sha256", check_rc=1)
        run("verify", "ok.sfv", "empty.sha256", check_rc=1)
        run("verify", check_rc=2)
        # hash of a folder: every file, names relative to the folder's parent
        p = run("hash", "sub", check_rc=0)
        check(f"{want(hdir / 'sub' / 'c.txt', 'sha256')} *sub/c.txt" in p.stdout.decode("utf-8").replace("\\", "/"), "hash of a folder: its files, with the folder in the name")
    finally:
        os.chdir(root)
    # entries of an archive: same values as the files they came from, no extraction to disk
    harc = root / "hashing.zip"
    run("a", str(harc), str(hdir / "sub"), check_rc=0)
    p = run("hash", "--in", str(harc), "-a", "sha256,crc32", check_rc=0)
    o = p.stdout.decode("utf-8")
    check(f"SHA256 (sub/c.txt) = {want(hdir / 'sub' / 'c.txt', 'sha256')}" in o and "CRC32 (sub/c.txt) = " in o, "hash --in: the entries of an archive")
    run("hash", "--in", str(harc), "nope/none.txt", check_rc=1)
    # checksum files made with the archive, and checked against it
    sarc = root / "sums" / "pack.zip"
    sarc.parent.mkdir(exist_ok=True)
    p = run("a", str(sarc), str(hdir / "sub"), "--sums-archive", "--sums-files", check_rc=0)
    check((sarc.parent / "pack.zip.sha256").exists() and (sarc.parent / "pack.sha256").exists(), "a --sums-archive --sums-files: both checksum files are next to the archive")
    check(f"{want(sarc, 'sha256')} *pack.zip" in (sarc.parent / "pack.zip.sha256").read_text(encoding="utf-8"), "the checksum of the archive is of the archive file")
    check(f"{want(hdir / 'sub' / 'c.txt', 'sha256')} *sub/c.txt" in (sarc.parent / "pack.sha256").read_text(encoding="utf-8"), "the checksum file of the files has the names of the archive")
    p = run("verify", str(sarc.parent / "pack.zip.sha256"), check_rc=0)
    p = run("verify", str(sarc.parent / "pack.sha256"), "--in", str(sarc), check_rc=0)
    check("All 1 files match" in p.stdout.decode("utf-8"), "verify --in: the files inside the archive match")
    # names that need a prefix: the checksum file of the files inside (made one level deeper)
    (sarc.parent / "inner.sha256").write_text(f"{want(hdir / 'sub' / 'c.txt', 'sha256')} *c.txt\n", encoding="utf-8")
    run("verify", str(sarc.parent / "inner.sha256"), "--in", str(sarc), check_rc=0)
    (sarc.parent / "odd.sha256").write_text(f"{want(hdir / 'sub' / 'c.txt', 'sha256')} *x/y/z/q/c.txt\n", encoding="utf-8")
    p = run("verify", str(sarc.parent / "odd.sha256"), "--in", str(sarc), check_rc=1)
    check("--strip" in p.stdout.decode("utf-8"), "verify --in: names that cannot be matched suggest --strip / --add-prefix")
    run("verify", str(sarc.parent / "odd.sha256"), "--in", str(sarc), "--strip", "x/y/z/q/", "--add-prefix", "sub/", check_rc=0)
    run("a", str(root / "sums" / "none.zip"), str(hdir / "sub"), check_rc=0)
    check(not (root / "sums" / "none.zip.sha256").exists() and not (root / "sums" / "none.sha256").exists(), "a without the options: no checksum files")

    # the diagnostic log: errors are always written, the detailed lines with LYCAN_LOG=debug; one file for every process
    logdir = root / "diag"
    env = dict(os.environ, LYCAN_LOG_DIR=str(logdir), LYCAN_SETTINGS_DIR=str(root / "no_settings"))   # (the settings of this machine must not matter)
    env.pop("LYCAN_LOG", None)
    subprocess.run([exe, "l", str(root / "nope.zip")], capture_output=True, env=env)   # fails: a warning is written without any switch
    text = (logdir / "lycanzip.log").read_text(encoding="utf-8-sig") if (logdir / "lycanzip.log").exists() else ""
    check(" cli] WARN  cli: l ended with exit code 1" in text and " INFO " not in text, "log: a failing command is in the log without any switch (and no detailed line is)")
    subprocess.run([exe, "l", str(harc)], capture_output=True, env=dict(env, LYCAN_LOG="debug"))
    text = (logdir / "lycanzip.log").read_text(encoding="utf-8-sig")
    check(" cli] INFO  cli: l" in text, "log: LYCAN_LOG=debug adds the detailed lines")
    p = subprocess.run([exe, "info"], capture_output=True, env=env)
    check(str(logdir) in p.stdout.decode("utf-8", errors="replace"), "info: tells where the log is")

    # does not exist / not an archive
    run("l", str(root / "nope.zip"), check_rc=1)
    junk = root / "junk.zip"
    junk.write_bytes(os.urandom(2000))
    run("l", str(junk), check_rc=1)
finally:
    shutil.rmtree(root, ignore_errors=True)

print("FAILURES:", failures)
sys.exit(1 if failures else 0)
