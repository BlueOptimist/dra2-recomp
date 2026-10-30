"""Find parameters typed narrower than the stack slot the code reads.

Usage: python find_narrow_params.py <decomp_dir> <out.tsv>

A parameter declared short/char/byte whose whole 4-byte slot is also read through its address
(*(undefined4 *)&param_2, or the split name _param_2) is really 4 bytes wide: recompiled, callers pass only the
narrow value and the rest is lost. Writes address, name, parameter and declared type for WidenParams.java.
"""
import glob
import os
import re
import sys

HEADER = re.compile(r"^// ([0-9a-f]{8}) (\S+) \(\d+ bytes\)$", re.M)
NARROW = re.compile(r"\b(short|ushort|char|byte|undefined2|undefined1|bool)\s+(param_\d+)\b")


def main():
    decomp, out = sys.argv[1], sys.argv[2]
    rows = []
    for path in glob.glob(os.path.join(decomp, "src", "**", "*.c"), recursive=True):
        parts = HEADER.split(open(path, encoding="latin-1").read())
        for i in range(1, len(parts), 3):
            body = parts[i + 2]
            sig = body.split("{", 1)[0]
            for t, p in NARROW.findall(sig):
                if re.search(r"\*\((?:\w+4|int|uint) \*\)&" + p + r"\b|\b_" + p + r"\b", body):
                    rows.append((parts[i], parts[i + 1], p, t))
    with open(out, "w") as f:
        f.write("address\tname\tparameter\ttype\n")
        for r in sorted(rows):
            f.write("\t".join(r) + "\n")
    print(f"narrow parameters read as a whole slot: {len(rows)} -> {out}")


if __name__ == "__main__":
    main()
