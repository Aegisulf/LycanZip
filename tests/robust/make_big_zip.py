#!/usr/bin/env python3
"""Writes a zip with many small entries (for the GUI stress test): make_big_zip.py <out.zip> <count> flat|nested|deep
  flat   : every file in one folder  (big/file000001.txt)
  nested : folders of 100 files      (big/d0001/file000001.txt)
  deep   : a chain of folders with a few files in each (big/l1/l2/.../file.txt, depth 40), the rest flat
"""
import sys
import zipfile

out, count, layout = sys.argv[1], int(sys.argv[2]), sys.argv[3]
with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED, allowZip64=True) as z:
    for i in range(count):
        if layout == "flat":
            name = "big/file%06d.txt" % i
        elif layout == "nested":
            name = "big/d%04d/file%06d.txt" % (i // 100, i)
        else:
            name = ("big/" + "/".join("l%d" % k for k in range(1, 41)) + "/file%06d.txt" % i) if i % 10 == 0 else "big/file%06d.txt" % i
        z.writestr(name, "x%d" % i)
print("wrote", out, count, layout)
