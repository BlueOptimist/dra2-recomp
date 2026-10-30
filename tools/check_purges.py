"""Compare each function's signature with how many argument bytes the machine code pops.

Usage: python check_purges.py <decomp_dir> <text.dis> <out.tsv> [text base, default 11000]

<text.dis> is llvm-objdump output for the XBE's .text (assembled from .incbin, so its addresses are offsets from
the section start: the base is added). A callee's "ret N" removes N bytes of
arguments; its signature implies a number too (cdecl 0, __stdcall every stack argument, __thiscall all but "this",
__fastcall all but the first two). When they disagree the decompiler tracks the stack wrong in every caller: after
the call its offsets are shifted by the difference, so one stack variable gets two names, or the wrong parameter is
passed (FUN_0013eab0 handed param_1 to the stream reader where the code pushes its second parameter).
Writes address, name, convention, signature pop, machine-code pop, signature, for every mismatch.
"""
import bisect
import glob
import os
import re
import sys

HEADER = re.compile(r"^// ([0-9a-f]{8}) (\S+) \((\d+) bytes\)$", re.M)
RET = re.compile(r"^\s*([0-9a-f]+):\s+retl?(?:\s+\$0x([0-9a-f]+))?\s*$")
WIDE = re.compile(r"\b(?:double|longlong|ulonglong|undefined8|float8|__int64)\b(?!\s*\*)")


def params_of(sig):
    open_, close = sig.find("("), sig.rfind(")")
    body = sig[open_ + 1:close].strip() if open_ >= 0 else ""
    if body in ("", "void"):
        return []
    parts, depth, cur = [], 0, ""
    for ch in body:
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
            continue
        depth += ch == "("
        depth -= ch == ")"
        cur += ch
    return parts + [cur.strip()]


def main():
    decomp, dis, out = sys.argv[1], sys.argv[2], sys.argv[3]
    base = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x11000
    rets = []
    for line in open(dis, encoding="latin-1"):
        m = RET.match(line)
        if m:
            rets.append((base + int(m.group(1), 16), int(m.group(2), 16) if m.group(2) else 0))
    rets.sort()
    addrs = [a for a, _ in rets]
    rows, checked = [], 0
    for path in sorted(glob.glob(os.path.join(decomp, "src", "**", "*.c"), recursive=True)):
        parts = HEADER.split(open(path, encoding="latin-1").read())
        for i in range(1, len(parts), 4):
            addr, name, size, body = int(parts[i], 16), parts[i + 1], int(parts[i + 2]), parts[i + 3]
            lines = [l for l in body.split("{", 1)[0].splitlines() if l.strip() and not l.lstrip().startswith(("/*", "*"))]
            sig = " ".join(l.strip() for l in lines)
            if "(" not in sig or "..." in sig:
                continue
            conv = ("thiscall" if "__thiscall" in sig else "fastcall" if "__fastcall" in sig
                    else "stdcall" if "__stdcall" in sig else "cdecl")
            params = [p for p in params_of(sig) if not re.search(r"\bin_e[a-d]x\b|\bin_e[sd]i\b", p)]
            stack = params
            if conv == "thiscall":
                stack = params[1:]
            elif conv == "fastcall":
                regs = 0
                stack = []
                for p in params:
                    if regs < 2 and not WIDE.search(p):
                        regs += 1
                    else:
                        stack.append(p)
            expected = 0 if conv == "cdecl" else sum(8 if WIDE.search(p) else 4 for p in stack)
            lo = bisect.bisect_left(addrs, addr)
            hi = bisect.bisect_left(addrs, addr + size)
            pops = {rets[k][1] for k in range(lo, hi)}
            if len(pops) != 1:
                continue  # no return found, or several (tail jumps, data in code): not conclusive
            checked += 1
            actual = pops.pop()
            if actual != expected:
                rows.append((parts[i], name, conv, expected, actual, sig))
    with open(out, "w") as f:
        f.write("address\tname\tconvention\tsignature_pop\tcode_pop\tsignature\n")
        for r in rows:
            f.write("\t".join(map(str, r)) + "\n")
    print(f"functions checked: {checked}, pop mismatches: {len(rows)} -> {out}")


if __name__ == "__main__":
    main()
