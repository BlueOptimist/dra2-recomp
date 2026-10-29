"""Find void functions whose callers use the value they leave in EAX / ST0.

Usage: python find_lost_returns.py <decomp_dir> <lost_returns.tsv>

When a function is typed void but a caller uses its result, the decompiler shows the result as extraout_EAX (or
extraout_ST0 for x87) after the call. Recompiled, that is an uninitialised local: a constructor's "this" result, an
allocator's pointer, become garbage. For each such use this finds the call before it, and writes the callees that
are still void with how many uses point at them (ghidra_scripts/SetLostReturns.java gives them a return type).
Rows already in the table are kept. Re-export afterwards; a new export can expose more, so repeat until none.
"""
import collections
import csv
import glob
import os
import re
import sys

HEADER = re.compile(r"^// ([0-9a-f]{8}) (\S+) \(\d+ bytes\)$", re.M)
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
NOT_CALLS = {"if", "while", "for", "switch", "return", "sizeof", "__typeof__", "CONCAT11", "CONCAT22", "CONCAT31",
             "CONCAT13", "CONCAT12", "CONCAT21", "CONCAT14", "CONCAT24", "SUB41", "SUB42", "ZEXT14", "SEXT14"}


def main():
    decomp, out = sys.argv[1], sys.argv[2]
    void_funcs = {}  # name -> address, for functions defined as void
    bodies = []
    for path in glob.glob(os.path.join(decomp, "src", "**", "*.c"), recursive=True):
        parts = HEADER.split(open(path, encoding="latin-1").read())
        for i in range(1, len(parts), 3):
            addr, name, body = parts[i], parts[i + 1], parts[i + 2]
            sig = next((l for l in body.splitlines() if l.strip() and not l.lstrip().startswith(("/*", "*", "//"))), "")
            if re.match(r"\s*void\s", sig):
                c_name = re.search(r"([A-Za-z_][\w:]*)\s*\(", sig)
                if c_name:
                    void_funcs[c_name.group(1).replace("::", "__")] = addr
            bodies.append(body)

    uses = collections.Counter()
    kinds = {}
    # Wrappers: "void f(x) { g(x); return; }" where g returns a value. MSVC compiled them as a tail call, so f returns
    # what g leaves in EAX; called only through vtables (an allocator's vf01 forwarding to the heap), no caller
    # shows extraout_EAX, so the use count above can't see them.
    returns_value = {}
    for path in glob.glob(os.path.join(decomp, "src", "**", "*.c"), recursive=True) + \
            glob.glob(os.path.join(decomp, "lib", "**", "*.c"), recursive=True):
        parts = HEADER.split(open(path, encoding="latin-1").read())
        for i in range(1, len(parts), 3):
            sig = next((l for l in parts[i + 2].splitlines()
                        if l.strip() and not l.lstrip().startswith(("/*", "*", "//"))), "")
            m = re.match(r"\s*(.+?)\s*\b([A-Za-z_][\w:]*)\s*\(", sig)
            if m and not re.fullmatch(r"(?:__\w+\s+)*void(?:\s+__\w+)*", m.group(1)):
                returns_value[m.group(2).replace("::", "__")] = "float10" if "float10" in m.group(1) else "undefined4"
    for body in bodies:
        statements = [l.strip() for l in body.split("{", 1)[-1].splitlines()
                      if l.strip() and l.strip() not in ("}", "return;")
                      and not re.match(r"[\w ]+\*?\s*\w+;$", l.strip())]
        sig = next((l for l in body.splitlines() if l.strip() and not l.lstrip().startswith(("/*", "*", "//"))), "")
        name = re.search(r"([A-Za-z_][\w:]*)\s*\(", sig)
        if len(statements) == 1 and name and re.match(r"\s*void\s", sig):
            call = re.fullmatch(r"([A-Za-z_][\w:]*)\s*\(.*\);", statements[0])
            callee = call.group(1).replace("::", "__") if call else None
            own = name.group(1).replace("::", "__")
            if callee in returns_value and own in void_funcs:
                uses[own] += 1
                kinds[own] = returns_value[callee]
    for body in bodies:
        last_call = None
        for line in body.splitlines():
            m = re.search(r"\bextraout_(EAX|ST0)\w*", line)
            if m and last_call and not re.search(r"\b" + re.escape(m.group(0)) + r"\s*;\s*$", line.split("=")[0]):
                if last_call in void_funcs:
                    uses[last_call] += 1
                    kinds[last_call] = "float10" if m.group(1) == "ST0" else "undefined4"
            calls = [c for c in CALL.findall(line) if c not in NOT_CALLS and not c.startswith("extraout_")]
            if calls:
                last_call = calls[0].replace("::", "__")

    rows = {}
    if os.path.exists(out):
        for r in csv.DictReader(open(out), delimiter="\t"):
            rows[r["address"]] = r
    added = 0
    for name, n in uses.items():
        addr = void_funcs[name]
        if addr not in rows:
            added += 1
        rows[addr] = {"address": addr, "name": name, "return": kinds[name], "uses": str(n)}
    with open(out, "w") as f:
        f.write("address\tname\treturn\tuses\n")
        for addr in sorted(rows):
            r = rows[addr]
            f.write(f"{r['address']}\t{r['name']}\t{r['return']}\t{r['uses']}\n")
    print(f"void functions whose result is used: {len(uses)} ({added} new) -> {out}")


if __name__ == "__main__":
    main()
