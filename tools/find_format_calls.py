"""Call-site overrides for printf-style calls: analysis/format_calls.tsv.

Usage: python find_format_calls.py <text.dis> <default.xbe> <varargs.tsv> <out.tsv> [text base, default 11000]

The variadic functions (analysis/varargs.tsv: the game's sprintf FUN_000541d0, its log and debug printers) were
decompiled with only their named parameters at every call: "sprintf(buf, "loc/%s/menus/level.res")" lost the
language string, so the background level's file name was garbage and the game reported a damaged disc. This does
what Ghidra's variadic-override analysis would: for each call, the format string is the last named argument, pushed
as an immediate .rdata address; its conversions give the number of extra arguments (a %f's double takes two stack
slots, a '*' width one more).

Output rows are in the analysis/call_overrides.tsv format (ApplyCallOverrides.java): call address, __cdecl, int, the
named parameters as pointers, the extra ones as undefined4 (bit copies), and a note.
"""
import csv
import re
import struct
import sys

INSN = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*?)\s*$")
CONVERSION = re.compile(r"%([-+ #0]*)(\*|\d+)?(?:\.(\*|\d+))?(hh|h|ll|l|L|I64|I32|I)?([diouxXeEfgGcCsSpn%])")


def xbe_reader(path):
    data = open(path, "rb").read()
    base, = struct.unpack_from("<I", data, 0x104)
    n, sect = struct.unpack_from("<II", data, 0x11C)
    sections = []
    for i in range(n):
        _, va, _, raw, rsize, _ = struct.unpack_from("<IIIIII", data, sect - base + i * 56)
        sections.append((va, raw, rsize))

    def string_at(addr):
        for va, raw, rsize in sections:
            if va <= addr < va + rsize:
                start = raw + addr - va
                end = data.index(b"\0", start)
                return data[start:end].decode("latin-1") if end - start < 512 else None
        return None
    return string_at


def stack_slots(fmt):
    """Extra stack dwords a format string's conversions consume, or None if it isn't plausibly a format."""
    slots = 0
    for flags, width, precision, size, conv in CONVERSION.findall(fmt):
        if conv == "%":
            continue
        slots += (width == "*") + (precision == "*")
        slots += 2 if conv in "eEfgG" or size in ("ll", "I64") else 1
    return slots


def main():
    dis, xbe, varargs, out = sys.argv[1:5]
    base = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0x11000
    string_at = xbe_reader(xbe)
    named = {int(r["address"], 16): int(r["named_params"]) for r in csv.DictReader(open(varargs), delimiter="\t")}
    insns = []
    for line in open(dis, encoding="latin-1"):
        m = INSN.match(line)
        if m and not m.group(2).startswith("<"):
            insns.append((base + int(m.group(1), 16), m.group(2), m.group(3).split("#")[0].strip()))
    rows = []
    for k, (addr, op, args) in enumerate(insns):
        if op != "calll":
            continue
        t = re.match(r"0x([0-9a-f]+)", args)
        if not t:
            continue
        target = (int(t.group(1), 16) + base) & 0xffffffff
        if target not in named:
            continue
        n = named[target]
        # the pushes before the call, nearest first: the n-th is the format string
        pushes, j = [], k - 1
        while j >= 0 and k - j <= 24 and len(pushes) < n:
            jop, jargs = insns[j][1], insns[j][2]
            if jop == "pushl":
                pushes.append(jargs)
            elif jop in ("calll", "retl", "jmp") or jop.startswith("j") or (jop == "addl" and jargs.endswith("%esp")):
                break
            j -= 1
        if len(pushes) < n:
            continue
        f = re.fullmatch(r"\$0x([0-9a-f]+)", pushes[n - 1])
        fmt = string_at(int(f.group(1), 16)) if f else None
        if fmt is None:
            continue
        slots = stack_slots(fmt)
        if not slots:
            continue
        params = ",".join(["pointer"] * n + ["undefined4"] * slots)
        rows.append((f"{addr:08x}", "__cdecl", "int", params,
                     f"call to {target:08x} with format \"{fmt[:60]}\": {slots} extra argument(s)"))
    with open(out, "w", newline="") as f:
        f.write("call\tconvention\treturn\tparams\tnote\n")
        for r in rows:
            f.write("\t".join(r) + "\n")
    print(f"printf-style calls given their format's arguments: {len(rows)} -> {out}")


if __name__ == "__main__":
    main()
