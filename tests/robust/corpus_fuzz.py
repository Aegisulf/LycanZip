#!/usr/bin/env python3
"""Mutation corpus test: build seed archives, damage them in many ways and run the CLI on every result.

A run is a failure when the CLI crashes (NTSTATUS exit code), hangs (timeout) or leaves files outside
the output folder. Ordinary errors (exit code 1/2/3) are the expected outcome for damaged input.

Usage: corpus_fuzz.py <lycanzip-cli.exe> [--seed N] [--per-seed N] [--workdir DIR] [--keep] [--timeout S]
Needs 7z.exe on PATH (to build the seed archives).
"""
import argparse
import concurrent.futures as cf
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile


def run(cmd, timeout, cwd=None):
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, cwd=cwd)
        return p.returncode, False
    except subprocess.TimeoutExpired:
        return None, True


def make_seeds(d, sevenz):
    """Return {name: path}. Content is small but varied (text, binary, nested folders, unicode names)."""
    src = os.path.join(d, "src")
    os.makedirs(os.path.join(src, "sub", "deep"), exist_ok=True)
    rnd = random.Random(1)
    with open(os.path.join(src, "a.txt"), "w", encoding="utf-8") as f:
        f.write("hello world\n" * 500)
    with open(os.path.join(src, "sub", "b.bin"), "wb") as f:
        f.write(bytes(rnd.getrandbits(8) for _ in range(20000)))
    with open(os.path.join(src, "sub", "deep", "c.txt"), "w", encoding="utf-8") as f:
        f.write("deep " * 3000)
    with open(os.path.join(src, "\u4e2d\u6587 \u540d\u7a31.txt"), "w", encoding="utf-8") as f:
        f.write("unicode name")
    with open(os.path.join(src, "empty.txt"), "wb"):
        pass
    seeds = {}

    def sz(name, *args):
        out = os.path.join(d, name)
        subprocess.run([sevenz, "a", "-bd", "-y", out, *args], stdout=subprocess.DEVNULL, check=True, cwd=src)
        seeds[name] = out

    sz("s_zip_deflate.zip", "-tzip", ".")
    sz("s_zip_store.zip", "-tzip", "-mx=0", ".")
    sz("s_zip_bzip2.zip", "-tzip", "-mm=BZip2", ".")
    sz("s_zip_aes.zip", "-tzip", "-ppw", "-mem=AES256", ".")
    sz("s_zip_zipcrypto.zip", "-tzip", "-ppw", "-mem=ZipCrypto", ".")
    sz("s_7z_lzma2.7z", "-t7z", ".")
    sz("s_7z_solid.7z", "-t7z", "-ms=on", ".")
    sz("s_7z_nosolid.7z", "-t7z", "-ms=off", ".")
    sz("s_7z_enc_names.7z", "-t7z", "-ppw", "-mhe=on", ".")
    sz("s_7z_ppmd.7z", "-t7z", "-m0=PPMd", ".")
    sz("s_tar.tar", "-ttar", ".")
    tar = seeds["s_tar.tar"]
    for ext, t in (("tar.gz", "gzip"), ("tar.xz", "xz"), ("tar.bz2", "bzip2")):
        out = os.path.join(d, "s_tar." + ext)
        subprocess.run([sevenz, "a", "-bd", "-y", "-t" + t, out, tar], stdout=subprocess.DEVNULL, check=True)
        seeds["s_tar." + ext] = out
    # Legacy zip with names in cp950 and no UTF-8 flag (the encoding detector's input)
    p = os.path.join(d, "s_zip_cp950.zip")
    with zipfile.ZipFile(p, "w") as z:
        for n in ("\u4e2d\u6587.txt", "\u8cc7\u6599\u593e/\u6a94\u6848.txt", "\u6e2c\u8a66\u8cc7\u6599.dat"):
            zi = zipfile.ZipInfo(n.encode("cp950").decode("cp437"))
            z.writestr(zi, b"x" * 100)
    seeds["s_zip_cp950.zip"] = p
    # Multi-volume
    vol_src = os.path.join(d, "vol")
    os.makedirs(vol_src, exist_ok=True)
    with open(os.path.join(vol_src, "v.bin"), "wb") as f:
        f.write(bytes(rnd.getrandbits(8) for _ in range(200000)))
    subprocess.run([sevenz, "a", "-bd", "-y", "-v64k", "-mx=0", os.path.join(d, "s_vol.7z"), "v.bin"], stdout=subprocess.DEVNULL, check=True, cwd=vol_src)
    seeds["s_vol.7z.001"] = os.path.join(d, "s_vol.7z.001")
    return seeds


def mutations(data, rnd, n):
    """Yield (label, bytes). Mix of truncation, bit flips, zeroing, garbage and header-field splats."""
    size = len(data)
    out = []
    # Truncations: structured points + random
    pts = {0, 1, 2, 3, 4, 8, 16, 22, 30, 32, 64, 128, size - 1, size - 2, size - 4, size - 8, size - 22, size - 32, size - 64, size // 2, size // 3}
    for _ in range(max(4, n // 8)):
        pts.add(rnd.randrange(0, max(1, size)))
    for p in sorted(pts):
        if 0 <= p < size:
            out.append((f"trunc{p}", data[:p]))
    # Bit/byte flips: concentrated in the first and last 256 bytes (headers / central directory) and anywhere
    def flip(buf, k, lo, hi):
        b = bytearray(buf)
        for _ in range(k):
            i = rnd.randrange(lo, max(lo + 1, min(hi, len(b))))
            if i < len(b):
                b[i] ^= 1 << rnd.randrange(8) if rnd.random() < 0.5 else rnd.randrange(256)
        return bytes(b)
    for i in range(n):
        r = rnd.random()
        if r < 0.35:
            out.append((f"head{i}", flip(data, rnd.randrange(1, 6), 0, 256)))
        elif r < 0.7:
            out.append((f"tail{i}", flip(data, rnd.randrange(1, 6), max(0, size - 256), size)))
        else:
            out.append((f"any{i}", flip(data, rnd.randrange(1, 12), 0, size)))
    # Zero or 0xFF a window
    for i in range(max(4, n // 10)):
        a = rnd.randrange(0, max(1, size - 1))
        w = rnd.choice((1, 2, 4, 8, 16, 64))
        fill = rnd.choice((0, 255))
        b = bytearray(data)
        b[a:a + w] = bytes([fill]) * min(w, size - a)
        out.append((f"fill{i}", bytes(b)))
    # Garbage appended / prepended, duplicated body
    out.append(("append_garbage", data + bytes(rnd.getrandbits(8) for _ in range(300))))
    out.append(("prepend_garbage", bytes(rnd.getrandbits(8) for _ in range(300)) + data))
    out.append(("double", data + data))
    # Huge 32-bit fields: overwrite a random aligned dword with an extreme value
    for i in range(max(4, n // 10)):
        a = rnd.randrange(0, max(1, size - 4))
        b = bytearray(data)
        b[a:a + 4] = struct.pack("<I", rnd.choice((0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x10000, 0xFFFF)))
        out.append((f"dword{i}", bytes(b)))
    return out


def trial(cli, path, base, timeout, work):
    """Run l / t / x on one mutated file. Returns a list of problems (empty = fine)."""
    problems = []
    outdir = os.path.join(work, "o_" + os.path.basename(base))
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir)
    before = set(os.listdir(os.path.dirname(path)))
    for name, cmd in (("l", [cli, "l", path]), ("t", [cli, "t", path, "-p", "pw"]),
                      ("x", [cli, "x", path, "-o", outdir, "-y", "-p", "pw"]),
                      ("x-enc", [cli, "x", path, "-o", outdir, "-y", "--encoding", "big5"])):
        rc, hung = run(cmd, timeout)
        if hung:
            problems.append(f"{name}: timeout")
        elif rc is not None and (rc < 0 or rc >= 0xC0000000 or rc > 3):
            problems.append(f"{name}: crash/odd exit code {rc & 0xFFFFFFFF:#x}")
    after = set(os.listdir(os.path.dirname(path)))
    if after - before:
        problems.append("left files next to the archive: " + ", ".join(sorted(after - before))[:200])
    leftover = [f for f in os.listdir(outdir) if f.endswith(".lycantmp")]
    if leftover:
        problems.append("temp files in output: " + ", ".join(leftover))
    shutil.rmtree(outdir, ignore_errors=True)
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cli")
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--per-seed", type=int, default=60)
    ap.add_argument("--workdir")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=float, default=30)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--crash-dir", help="copy failing inputs here")
    ap.add_argument("--seeds-only", metavar="DIR", help="only write the seed archives to DIR (a starting corpus for libFuzzer) and exit")
    a = ap.parse_args()
    sevenz = shutil.which("7z") or r"C:\Program Files\7-Zip\7z.exe"
    work = a.workdir or tempfile.mkdtemp(prefix="lycan_corpus_")
    os.makedirs(work, exist_ok=True)
    seeds = make_seeds(work, sevenz)
    if a.seeds_only:
        os.makedirs(a.seeds_only, exist_ok=True)
        for sname, spath in seeds.items():
            shutil.copy(spath, os.path.join(a.seeds_only, sname))
        print(f"wrote {len(seeds)} seeds to {a.seeds_only}")
        return 0
    rnd = random.Random(a.seed)
    jobs = []
    mdir = os.path.join(work, "m")
    os.makedirs(mdir, exist_ok=True)
    for sname, spath in seeds.items():
        with open(spath, "rb") as f:
            data = f.read()
        ext = ".7z.001" if sname.endswith(".001") else os.path.splitext(sname)[1]
        if sname.endswith((".tar.gz", ".tar.xz", ".tar.bz2")):
            ext = "." + ".".join(sname.split(".")[-2:])
        for label, buf in mutations(data, rnd, a.per_seed):
            sub = os.path.join(mdir, f"{len(jobs):05d}")
            os.makedirs(sub)
            p = os.path.join(sub, "m" + ext)
            with open(p, "wb") as f:
                f.write(buf)
            jobs.append((sname, label, p))
    print(f"{len(seeds)} seeds, {len(jobs)} damaged files, workdir {work}", flush=True)
    t0 = time.time()
    bad = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(trial, a.cli, p, f"{i}", a.timeout, work): (sname, label, p) for i, (sname, label, p) in enumerate(jobs)}
        done = 0
        for fu in cf.as_completed(futs):
            sname, label, p = futs[fu]
            done += 1
            probs = fu.result()
            if probs:
                bad.append((sname, label, p, probs))
                print(f"PROBLEM {sname} {label}: {'; '.join(probs)}", flush=True)
            if done % 200 == 0:
                print(f"  {done}/{len(jobs)} ({time.time() - t0:.0f}s)", flush=True)
    if a.crash_dir and bad:
        os.makedirs(a.crash_dir, exist_ok=True)
        for sname, label, p, _ in bad:
            shutil.copy(p, os.path.join(a.crash_dir, f"{sname}.{label}{os.path.splitext(p)[1]}"))
    print(f"done in {time.time() - t0:.0f}s: {len(jobs)} files, {len(bad)} with problems")
    if not a.keep and a.workdir is None:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
