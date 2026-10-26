#!/usr/bin/env python3
"""Write starting corpora for the libFuzzer targets: <out>/paths, <out>/encoding, <out>/open."""
import os
import subprocess
import sys

NAMES = [
    "plain.txt", "dir/sub/file.txt", "../../escape.txt", "..\\..\\escape.txt", "C:\\abs.txt", "\\\\server\\share\\x", "/abs/x",
    "con.txt", "AUX", "trail.", "trail ", "a:b", "x::$DATA", "a<b>c", "ctrl\x01.txt", "\u202eRTL", "\u4e2d\u6587/\u540d\u7a31.txt",
    "CON .txt", "COM\u00b9.txt", "lpt\u00b3", "CONIN$", "conout$.log", "\uff0e\uff0e/x", "..\u2215x", "PROGRA~1/x", "\\\\?\\C:\\x", "\\??\\C:\\x",
    "d/" * 40 + "deep", "a" * 260, "./x", "x/./y", "x//y", "x/../y", "....", " ", "",
]


def main():
    out = sys.argv[1]
    for sub in ("paths", "encoding", "open", "iso", "update"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    # paths: UTF-16LE, names separated by newline
    for i, n in enumerate(NAMES):
        with open(os.path.join(out, "paths", f"n{i:02d}"), "wb") as f:
            f.write(n.encode("utf-16-le"))
    with open(os.path.join(out, "paths", "all"), "wb") as f:
        f.write("\n".join(NAMES).encode("utf-16-le"))
    # iso: items [flags][size][UTF-16 name][0 0] (see fuzz_iso.cpp)
    def iso_item(flags, size, name):
        return bytes([flags, size]) + name.encode("utf-16-le") + b"\x00\x00"
    with open(os.path.join(out, "iso", "simple"), "wb") as f:
        f.write(iso_item(0, 5, "a.txt") + iso_item(1, 0, "dir") + iso_item(0, 9, "dir/b.txt") + iso_item(0, 3, "dir/sub/c.txt"))
    with open(os.path.join(out, "iso", "collide"), "wb") as f:
        f.write(b"".join(iso_item(0, 1, "same_prefix_%d.txt" % i) for i in range(30)))
    with open(os.path.join(out, "iso", "names"), "wb") as f:
        f.write(b"".join(iso_item(i % 2, 2, n) for i, n in enumerate(NAMES)))
    # update: [mode] then operations [kind][len][UTF-16 name] (rename has a second name)
    def upd(kind, *names):
        b = bytes([kind])
        for n in names:
            b += bytes([len(n)]) + n.encode("utf-16-le")
        return b
    with open(os.path.join(out, "update", "mix"), "wb") as f:
        f.write(bytes([3]) + upd(0, "a") + upd(1, "x.txt") + upd(2, "y.txt", "renamed.txt") + upd(3, "newdir/inner") + upd(4, "a/z.txt"))
    with open(os.path.join(out, "update", "sync"), "wb") as f:
        f.write(bytes([3]) + upd(0, "") + upd(1, "src"))
    # encoding: first byte selects the system code page, then names in several encodings
    samples = ["\u4e2d\u6587\u6a94\u6848.txt", "\u8cc7\u6599\u593e/\u6e2c\u8a66.dat", "\u65e5\u672c\u8a9e\u306e\u30d5\u30a1\u30a4\u30eb.txt", "\ud55c\uae00 \ud30c\uc77c.txt", "\u7b80\u4f53\u4e2d\u6587.txt"]
    for cp in ("big5", "gbk", "shift_jis", "euc_kr", "utf-8"):
        blob = bytearray([1 if cp == "big5" else 0])
        for s in samples:
            try:
                blob += s.encode(cp) + b"\n"
            except UnicodeEncodeError:
                blob += s.encode("utf-8") + b"\n"
        with open(os.path.join(out, "encoding", cp), "wb") as f:
            f.write(bytes(blob))
    # open: the seed archives of the corpus test, prefixed with the extension selector byte
    here = os.path.dirname(os.path.abspath(__file__))
    tmp = os.path.join(out, "_seeds")
    subprocess.run([sys.executable, os.path.join(here, "corpus_fuzz.py"), "unused", "--seeds-only", tmp], check=True, stdout=subprocess.DEVNULL)
    sel = {".zip": 0, ".7z": 1, ".tar": 2, ".tar.gz": 3, ".tar.xz": 4, ".tar.bz2": 5, ".7z.001": 10}
    for fn in os.listdir(tmp):
        for ext, b0 in sel.items():
            if fn.endswith(ext) and (ext != ".zip" or fn.endswith(".zip")):
                if ext == ".7z" and fn.endswith(".7z.001"):
                    continue
                with open(os.path.join(tmp, fn), "rb") as f:
                    data = f.read()
                if len(data) < (1 << 20):
                    with open(os.path.join(out, "open", fn), "wb") as g:
                        g.write(bytes([b0]) + data)
                break
    for fn in os.listdir(tmp):
        os.remove(os.path.join(tmp, fn))
    os.rmdir(tmp)
    print("seeds written to", out)


if __name__ == "__main__":
    main()
