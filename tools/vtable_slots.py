"""Summarise the virtual slots of a class family: for each slot of the base vtable, the base implementation, how many
subclasses override it, and a short digest of each body (size, strings, calls) from the decompiled code.

Usage: python vtable_slots.py <default.xbe> <classes.tsv> <class_names.tsv> <decomp_dir> <base class name> [slot ...]
With slot numbers given, prints the full decompiled base implementation and one override for each.
"""
import collections
import csv
import glob
import os
import re
import struct
import sys


def xbe_reader(path):
    data = open(path, "rb").read()
    base, = struct.unpack_from("<I", data, 0x104)
    n, sect = struct.unpack_from("<II", data, 0x11C)
    secs = []
    for i in range(n):
        _, va, vsize, raw, rsize = struct.unpack_from("<IIIII", data, sect - base + i * 56)
        secs.append((va, raw, rsize))

    def u32(va):
        for s_va, raw, rsize in secs:
            if s_va <= va < s_va + rsize:
                return struct.unpack_from("<I", data, raw + va - s_va)[0]
        return None
    return u32


def load_bodies(decomp):
    bodies = {}
    for p in glob.glob(os.path.join(decomp, "src", "**", "*.c"), recursive=True):
        text = open(p, encoding="latin-1").read()
        parts = re.split(r"\n// ([0-9a-f]{8}) (\S+) \((\d+) bytes\)\n", text)
        for i in range(1, len(parts) - 3, 4):
            bodies[int(parts[i], 16)] = (parts[i + 1], int(parts[i + 2]), parts[i + 3])
    return bodies


def digest(body):
    strings = re.findall(r'"([^"]{2,40})"', body)
    calls = [c for c in re.findall(r"\b([A-Za-z_][\w:]*)\(", body)
             if not c.startswith(("if", "while", "for", "switch", "return", "sizeof", "CONCAT", "SUB", "ZEXT"))]
    vcalls = re.findall(r"->(slot\d+_\w+|vf\d+)", body)
    return strings[:4], [c for c, _ in collections.Counter(calls).most_common(4)], sorted(set(vcalls))[:4]


def main():
    xbe, classes_tsv, names_tsv, decomp, base_name = sys.argv[1:6]
    show = [int(x) for x in sys.argv[6:]]
    u32 = xbe_reader(xbe)
    names = {r["class"]: r["name"] for r in csv.DictReader(open(names_tsv), delimiter="\t")}
    rows = list(csv.DictReader(open(classes_tsv), delimiter="\t"))
    by_name = {names.get(r["class"], r["class"]): r for r in rows}
    base = by_name[base_name]
    children = collections.defaultdict(list)
    for r in rows:
        for p in r["parents"].split(","):
            if p:
                children[p].append(r["class"])
    family, stack = [], [base["class"]]
    while stack:
        c = stack.pop()
        family.append(c)
        stack.extend(children[c])
    vt = {c: [u32(int(by_name[names.get(c, c)]["vtable"], 16) + 4 * i)
              for i in range(int(by_name[names.get(c, c)]["slots"]))] for c in family}
    bodies = load_bodies(decomp)
    base_vt = vt[base["class"]]
    print(f"{base_name}: {len(base_vt)} slots, family of {len(family)} classes")
    for i, impl in enumerate(base_vt):
        overrides = sorted({v[i] for c, v in vt.items() if c != base["class"] and i < len(v) and v[i] != impl})
        name, size, body = bodies.get(impl, ("?", 0, ""))
        s, calls, vcalls = digest(body)
        print(f"slot {i:02d} base {impl:06x} {name.split('::')[-1]:16} {size:5}B overrides={len(overrides):2} "
              f"calls={calls} vcalls={vcalls} str={s}")
        if i in show:
            print(body[:2500])
            if overrides:
                o = overrides[0]
                oname, osize, obody = bodies.get(o, ("?", 0, ""))
                print(f"--- override {o:06x} {oname} ({osize}B)\n{obody[:2000]}")


if __name__ == "__main__":
    main()
