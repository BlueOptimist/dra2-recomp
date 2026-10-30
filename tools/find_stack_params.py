"""Find functions that read stack arguments their signature doesn't declare.

Usage: python find_stack_params.py <decomp_dir> <out.tsv>

The decompiler shows such an argument as in_stack_00000004 (offset above the entry stack pointer). Recompiled it is
garbage: callers pass nothing there. Writes address, name and the highest such offset (hex) for
ghidra_scripts/AddStackParams.java, which declares the missing parameters.
"""
import glob
import os
import re
import sys

HEADER = re.compile(r"^// ([0-9a-f]{8}) (\S+) \(\d+ bytes\)$", re.M)


def main():
    decomp, out = sys.argv[1], sys.argv[2]
    rows = []
    for path in glob.glob(os.path.join(decomp, "src", "**", "*.c"), recursive=True):
        parts = HEADER.split(open(path, encoding="latin-1").read())
        for i in range(1, len(parts), 3):
            offsets = {int(o, 16) for o in re.findall(r"\bin_stack_(0000[0-9a-f]{4})\b", parts[i + 2])}
            if offsets:
                rows.append((parts[i], parts[i + 1], max(offsets)))
    with open(out, "w") as f:
        f.write("address\tname\thighest_offset\n")
        for addr, name, off in sorted(rows):
            f.write(f"{addr}\t{name}\t{off:x}\n")
    print(f"functions reading undeclared stack arguments: {len(rows)} -> {out}")


if __name__ == "__main__":
    main()
