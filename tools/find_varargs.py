"""Find the printf-style wrappers the decompiler doesn't know are variadic.

Usage: python find_varargs.py <decomp_dir> <out.tsv>

A cdecl function that takes the address just past its last named parameter, &stack0x(4 + 4 * named), and passes it
on (to vsnprintf and the like) is variadic: the rest of its arguments are the "...". Ghidra shows its calls with the
named arguments only, so the formatted values are lost. Writes address, name and the number of named parameters
for each such function not already declared with "..." (ghidra_scripts/SetVarargs.java applies them).
"""
import glob
import os
import re
import sys

HEADER = re.compile(r"^// ([0-9a-f]{8}) (\S+) \(\d+ bytes\)$", re.M)


def main():
    decomp, out = sys.argv[1], sys.argv[2]
    rows = []
    for path in sorted(glob.glob(os.path.join(decomp, "src", "**", "*.c"), recursive=True) +
                       glob.glob(os.path.join(decomp, "lib", "**", "*.c"), recursive=True)):
        parts = HEADER.split(open(path, encoding="latin-1").read())
        for i in range(1, len(parts), 3):
            addr, name, body = parts[i], parts[i + 1], parts[i + 2]
            lines = [l for l in body.splitlines() if l.strip() and not l.lstrip().startswith(("/*", "*", "//"))]
            if not lines:
                continue
            sig = lines[0]
            if "..." in sig or "__thiscall" in sig or "__fastcall" in sig or "__stdcall" in sig:
                continue
            m = re.search(r"\((.*)\)\s*$", sig)
            if not m:
                continue
            params = [p for p in m.group(1).split(",") if p.strip() not in ("", "void")]
            if not params:
                continue  # a format in no named parameter: not this pattern
            rest = f"&stack0x{4 + 4 * len(params):08x}"
            # passed on as an argument (the va_list), not just read
            if re.search(rf"\w\s*\([^;]*{re.escape(rest)}", body):
                rows.append((addr, name, len(params)))
    with open(out, "w") as f:
        f.write("address\tname\tnamed_params\n")
        for r in rows:
            f.write("\t".join(map(str, r)) + "\n")
    print(f"{len(rows)} variadic wrappers -> {out}")


if __name__ == "__main__":
    main()
