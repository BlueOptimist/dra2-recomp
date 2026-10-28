"""Find the Lua config keys that the engine reads by name, and their strings in default.xbe.

Usage: python lua_bindings.py <unpacked_dir> <default.xbe> <out.tsv>

The game's Lua scripts are almost entirely data tables (characterConfig.default = { walkSpeed = 5, ... }).
The C++ engine looks those keys up by string, so every key that also exists as a NUL-terminated string
in the XBE marks code that reads that table. Output columns: key, top-level table(s) it appears in,
number of uses in Lua, and the string's virtual address(es) in the XBE, ready for
ghidra_scripts/LabelLuaKeys.java.
"""
import os
import re
import struct
import sys

TOP = re.compile(r"^([A-Za-z_]\w*)(?:[.\[][^=]*)?\s*=(?!=)")
KEY = re.compile(r"(?<![\w.])([A-Za-z_]\w*)\s*=(?!=)")


def strip(text):
    text = re.sub(r"--[^\n]*", "", text)
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', '""', text)


def scan_lua(root):
    keys = {}  # key -> {"tables": set, "count": int}
    for dp, _, fns in os.walk(root):
        for fn in fns:
            if not fn.endswith(".lua"):
                continue
            depth, table = 0, None
            for line in strip(open(os.path.join(dp, fn), encoding="latin-1").read()).splitlines():
                stripped = line.strip()
                if depth == 0:
                    m = TOP.match(stripped)
                    if m and not stripped.startswith("local "):
                        table = m.group(1)
                        k = keys.setdefault(table, {"tables": set(), "count": 0})
                        k["tables"].add("<global>")
                        k["count"] += 1
                elif table:
                    for m in KEY.finditer(stripped):
                        k = keys.setdefault(m.group(1), {"tables": set(), "count": 0})
                        k["tables"].add(table)
                        k["count"] += 1
                depth += stripped.count("{") - stripped.count("}")
                depth = max(depth, 0)
    return keys


def xbe_strings(path):
    data = open(path, "rb").read()
    base, = struct.unpack_from("<I", data, 0x104)
    n_sect, sect_va = struct.unpack_from("<II", data, 0x11C)
    sections = []
    for i in range(n_sect):
        _, va, vsize, raw, rsize = struct.unpack_from("<IIIII", data, sect_va - base + i * 56)
        sections.append((va, raw, rsize))
    found = {}
    for m in re.finditer(rb"(?<=\0)[A-Za-z_]\w{1,63}(?=\0)", data):
        off = m.start()
        for va, raw, rsize in sections:
            if raw <= off < raw + rsize:
                found.setdefault(m.group().decode(), []).append(va + off - raw)
                break
    return found


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    keys = scan_lua(sys.argv[1])
    strings = xbe_strings(sys.argv[2])
    hits = sorted((k for k in keys if k in strings), key=lambda k: -keys[k]["count"])
    with open(sys.argv[3], "w") as out:
        out.write("key\ttables\tlua_uses\taddresses\n")
        for k in hits:
            out.write(f"{k}\t{','.join(sorted(keys[k]['tables']))}\t{keys[k]['count']}\t"
                      f"{','.join(f'0x{a:08X}' for a in strings[k])}\n")
    tables = {}
    for k in hits:
        for t in keys[k]["tables"]:
            tables[t] = tables.get(t, 0) + 1
    print(f"{len(keys)} distinct keys in Lua, {len(hits)} found as strings in the XBE")
    print("keys found per table:", ", ".join(f"{t}={n}" for t, n in sorted(tables.items(), key=lambda x: -x[1])[:15]))


if __name__ == "__main__":
    main()
