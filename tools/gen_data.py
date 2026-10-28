"""Re-emit the XBE's .rdata and .data sections as a relocatable object for the recompiled code.

Usage: python gen_data.py <xbe> <decomp_dir> <analysis_dir> <llvm_bin_dir>

Run after tools/build_objects.py (it reads build/obj/*.obj and build/obj_src/*.c) and after
DumpDataSymbols.java (analysis/data_symbols.tsv, analysis/data_refs.tsv).

The sections keep their original layout, so offsets between labels (arrays, structs spanning labels) still hold.
What moves is every pointer stored in them:
  - a pointer to a function the recompiled code defines becomes a relocation against that function's symbol
  - a pointer into .rdata/.data becomes a relocation against the section base + offset
  - a pointer anywhere else (code labels with no function, XDK library sections, the XBE header) keeps its raw
    Xbox address and is counted in the report: those still need a decision
Pointers are those Ghidra's analysis found (data_refs.tsv) plus every aligned word equal to a function entry the
recompiled code defines (vtables and tables Ghidra left undefined).
Every symbol the objects need that lies in these sections is defined at its address, under the exact decorated name
the objects use. Writes build/data/xbe_data.s, build/obj/xbe_data.obj and build/data/data_report.txt.
"""
import collections
import csv
import glob
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compile_check import FUNC_HEADER, PROTO_NAME  # noqa: E402

DATA_SECTIONS = (".rdata", ".data")
# names the Windows C runtime (and the compiler's own helpers in it) will provide when the .exe is linked
WINDOWS_CRT = set("""qsort memmove memcpy memset memcmp sprintf strcspn strncmp strncpy strrchr strchr strstr strlen
strcmp strcpy strcat rand srand isdigit isspace isalpha toupper tolower atol atoi atof atexit _stricmp _strnicmp
_vsnprintf vsprintf _copysign _controlfp ceil floor fread fwrite fopen fclose fseek ftell free malloc realloc calloc
log2 log log10 exp exp2 pow sqrt sin cos tan atan atan2 asin acos fabs fmod ldexp rint _allrem _allmul _alldiv
_aulldiv _aullrem _allshl _allshr _aullshr _chkstk _alloca_probe _fltused _ftol _ftol2 _ftol2_sse printf abort
exit _finite _strdup _cexit""".split())


def xbe_sections(path):
    data = open(path, "rb").read()
    base, = struct.unpack_from("<I", data, 0x104)
    n, sect = struct.unpack_from("<II", data, 0x11C)
    secs = {}
    for i in range(n):
        flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<IIIIII", data, sect - base + i * 56)
        name = data[name_va - base:data.index(b"\0", name_va - base)].decode()
        secs[name] = (va, vsize, data[raw:raw + rsize])
    return secs


def nm_raw(llvm_nm, obj):
    proc = subprocess.run([llvm_nm, "--format=posix", obj], capture_output=True, text=True, encoding="latin-1")
    defined, undefined = set(), set()
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            (undefined if parts[1] == "U" else defined).add(parts[0])
    return defined, undefined


def undecorate(raw):
    """i686 Windows C decoration -> C name: _name (cdecl), _name@N (stdcall), @name@N (fastcall)."""
    name = raw[1:] if raw[:1] in "_@" else raw
    return re.sub(r"@\d+$", "", name)


def main():
    xbe, decomp, analysis, llvm_bin = sys.argv[1:5]
    build = os.path.join(decomp, "build")
    llvm_nm = os.path.join(llvm_bin, "llvm-nm.exe")
    clang = os.path.join(llvm_bin, "clang.exe")
    secs = xbe_sections(xbe)

    # decorated symbols defined / needed by the compiled code
    defined, undefined = set(), set()
    for obj in glob.glob(os.path.join(build, "obj", "*.obj")):
        if os.path.basename(obj) == "xbe_data.obj":
            continue
        d, u = nm_raw(llvm_nm, obj)
        defined |= d
        undefined |= u
    undefined -= defined
    raw_by_cname = {}
    for raw in defined:
        raw_by_cname.setdefault(undecorate(raw), raw)

    # function address -> decorated symbol, from the compiled sources' own headers (after duplicate renaming)
    func_raw = {}
    for src in glob.glob(os.path.join(build, "obj_src", "**", "*.c"), recursive=True):
        lines = open(src, encoding="latin-1").read().split("\n")
        for i, line in enumerate(lines):
            m = FUNC_HEADER.match(line)
            if not m:
                continue
            for j in range(i + 1, min(i + 12, len(lines))):
                s = lines[j].strip()
                if s.startswith(("/*", "//", "*")) or not s:
                    continue
                names = PROTO_NAME.findall(s)
                if names and names[0] in raw_by_cname:
                    func_raw[int(m.group(1), 16)] = raw_by_cname[names[0]]
                    break
                if s == "{":
                    break

    # labels Ghidra knows in data, by name (for names without an address: luakey_*, X_vftable)
    label_addr = {}
    for r in csv.DictReader(open(os.path.join(analysis, "data_symbols.tsv")), delimiter="\t"):
        if r["kind"] == "label":
            label_addr.setdefault(r["name"], int(r["address"], 16))

    ranges = {name: (secs[name][0], secs[name][0] + secs[name][1]) for name in DATA_SECTIONS}

    def section_of(addr):
        for name, (lo, hi) in ranges.items():
            if lo <= addr < hi:
                return name
        return None

    # where each needed symbol lives
    labels = collections.defaultdict(list)  # address -> [decorated names]
    not_here = collections.Counter()
    for raw in sorted(undefined):
        c = undecorate(raw)
        addr = None
        m = re.search(r"_([0-9a-f]{8})$", c)
        if c.endswith("_vftable_data"):
            addr = label_addr.get(c[:-len("_data")])
        elif m and re.match(r"_?(?:DAT|PTR|s|u|UNK|BYTE|WORD|DWORD|FLOAT|DOUBLE)_", c):
            addr = int(m.group(1), 16)
        elif c.startswith("luakey_"):
            addr = label_addr.get(c)
        if addr is not None and section_of(addr):
            labels[addr].append(raw)
        else:
            not_here[re.sub(r"_[0-9a-f]{8}$|^_+", "", c).split("_")[0] or c] += 1

    # The Xbox kernel import table at the start of .rdata: one word per import (0x80000000 | ordinal), ending with 0.
    # Each entry gets a stub that reports the import by name and stops (port_kernel_unimplemented); the game never
    # calls the kernel itself, only the Xbox libraries the port replaces do.
    kernel_table = {}
    rdata_va, _, rdata_raw = secs[".rdata"]
    names_at = {}
    for r in csv.DictReader(open(os.path.join(analysis, "data_symbols.tsv")), delimiter="\t"):
        if r["kind"] == "label" and "." not in r["name"]:
            names_at.setdefault(int(r["address"], 16), r["name"])
    for k in range(0, 1024):
        w, = struct.unpack_from("<I", rdata_raw, 4 * k)
        if not w & 0x80000000:
            break
        kernel_table[rdata_va + 4 * k] = names_at.get(rdata_va + 4 * k, f"ordinal_{w & 0xffff}")

    # pointers stored in the sections
    pointers = {}
    for r in csv.DictReader(open(os.path.join(analysis, "data_refs.tsv")), delimiter="\t"):
        src, dst = int(r["from"], 16), int(r["to"], 16)
        if section_of(src):
            pointers[src] = dst
    func_entries = set(func_raw)
    for name in DATA_SECTIONS:
        va, vsize, raw = secs[name]
        for off in range(0, len(raw) - 3, 4):
            v, = struct.unpack_from("<I", raw, off)
            if v in func_entries:
                pointers.setdefault(va + off, v)

    out_dir = os.path.join(build, "data")
    os.makedirs(out_dir, exist_ok=True)
    stats = collections.Counter()
    unresolved = collections.Counter()
    asm = ["# Generated by tools/gen_data.py from default.xbe: .rdata and .data with relocated pointers.",
           "\t.text"]
    for name in DATA_SECTIONS:
        va, vsize, raw = secs[name]
        base_sym = "Lxbe_" + name.strip(".")
        asm.append(f'\t.section {name},"{"dr" if name == ".rdata" else "dw"}"')
        asm.append("\t.p2align 4")
        asm.append(f"{base_sym}:")
        label_offsets = sorted(a - va for a in labels if va <= a < va + vsize)
        label_set = set(label_offsets)
        pending = []

        def flush():
            for k in range(0, len(pending), 32):
                asm.append("\t.byte " + ",".join(str(b) for b in pending[k:k + 32]))
            pending.clear()

        off = 0
        while off < vsize:
            if off in label_set:
                flush()
                for sym in labels[va + off]:
                    asm.append(f"\t.globl {sym}")
                    asm.append(f"{sym}:")
            if off >= len(raw):  # zero-filled tail (.bss part of .data)
                flush()
                nxt = next((l for l in label_offsets if l > off), vsize)
                asm.append(f"\t.zero {nxt - off}")
                off = nxt
                continue
            if va + off in kernel_table and not any(off + k in label_set for k in (1, 2, 3)):
                flush()
                asm.append(f"\t.long Lkernel_{kernel_table[va + off]}")
                stats["kernel imports routed to reporting stubs"] += 1
                off += 4
                continue
            ptr = pointers.get(va + off)
            if ptr is not None and off + 4 <= len(raw) and not any(off + k in label_set for k in (1, 2, 3)) \
                    and struct.unpack_from("<I", raw, off)[0] == ptr:
                flush()
                if ptr in func_raw:
                    asm.append(f"\t.long {func_raw[ptr]}")
                    stats["code pointers relocated"] += 1
                elif section_of(ptr):
                    tsec = section_of(ptr)
                    asm.append(f"\t.long Lxbe_{tsec.strip('.')}+{ptr - secs[tsec][0]}")
                    stats["data pointers relocated"] += 1
                else:
                    asm.append(f"\t.long 0x{ptr:08x}  # unresolved: no recompiled symbol here")
                    owner = next((n for n, (v, s, _) in secs.items() if v <= ptr < v + s), "outside image")
                    unresolved[owner] += 1
                off += 4
                continue
            pending.append(raw[off])
            off += 1
        flush()
        stats[f"{name} bytes"] = vsize
        stats[f"{name} labels defined"] = sum(len(labels[a]) for a in labels if va <= a < va + vsize)

    # thunks: Ghidra's thunk_FUN_x is a lone JMP to FUN_x (the export skips thunks); same here, under the exact
    # decorated name the callers use, so arguments and stack cleanup pass straight through
    asm.append("\t.text")
    for raw in sorted(undefined):
        c = undecorate(raw)
        if c.startswith("thunk_") and c[len("thunk_"):] in raw_by_cname:
            asm += [f"\t.globl {raw}", f"{raw}:", f"\tjmp {raw_by_cname[c[len('thunk_'):]]}"]
            stats["thunks"] += 1

    for name in kernel_table.values():
        asm += [f"Lkernel_{name}:", f"\tpushl $Lkname_{name}", "\tcalll _port_kernel_unimplemented",
                f"Lkname_{name}:", f'\t.asciz "{name}"']

    s_path = os.path.join(out_dir, "xbe_data.s")
    with open(s_path, "w", encoding="latin-1") as f:
        f.write("\n".join(asm) + "\n")
    obj = os.path.join(build, "obj", "xbe_data.obj")
    target = next((a.split("=", 1)[1] for a in sys.argv[5:] if a.startswith("--target=")), "i686-pc-windows-msvc")
    proc = subprocess.run([clang, f"--target={target}", "-c", s_path, "-o", obj],
                          capture_output=True, text=True, encoding="latin-1")
    if proc.returncode:
        raise SystemExit("assembling failed:\n" + proc.stderr[:3000])

    data_defined, _ = nm_raw(llvm_nm, obj)
    still = sorted(undefined - data_defined)
    kinds = collections.Counter()
    for raw in still:
        c = undecorate(raw)
        if raw.startswith("__imp_"):
            kinds["Windows API import (kernel32 etc., provided by import libraries at link time)"] += 1
        elif re.match(r"lua_|luaL_|luaopen_", c):
            kinds["Lua 5.0 API (from the Lua 5.0 source, LUA_NUMBER float, at link time)"] += 1
        elif c in WINDOWS_CRT:
            kinds["C runtime (provided by the Windows CRT at link time)"] += 1
        elif re.match(r"(?:thunk_)?(?:FUN|LAB)_", c):
            kinds["code (FUN_/LAB_ not compiled)"] += 1
        elif re.match(r"_?(?:DAT|PTR|s|u|UNK)_", c):
            kinds["data label outside .rdata/.data"] += 1
        elif re.match(r"(?:D3D8|DSOUND|XAPILIB|D3DX|XGRAPHICS)__", c):
            kinds["Xbox system API"] += 1
        elif re.match(r"[a-z]{1,4}Ram[0-9a-f]{8}$|_(?:param|local)_|_\w+Stack_|stack0x|register0x", c):
            kinds["decompiler pseudo-variable"] += 1
        else:
            kinds["other"] += 1
    with open(os.path.join(out_dir, "data_report.txt"), "w") as f:
        for k, v in stats.items():
            f.write(f"{k}: {v}\n")
        f.write("raw pointers left (target section): " + ", ".join(f"{k} {v}" for k, v in unresolved.most_common())
                + "\n\nstill undefined after the data object: %d\n" % len(still))
        for k, v in kinds.most_common():
            f.write(f"  {v:6}  {k}\n")
        f.write("\n" + "\n".join(f"  {undecorate(r)}" for r in still) + "\n")
    for k, v in stats.items():
        print(f"{k}: {v}")
    print("raw pointers left (target section):", dict(unresolved.most_common()))
    print(f"undefined before: {len(undefined)}, after the data object: {len(still)}")
    for k, v in kinds.most_common():
        print(f"  {v:6}  {k}")


if __name__ == "__main__":
    main()
