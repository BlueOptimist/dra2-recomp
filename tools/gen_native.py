"""Generate the tables for the native build (port/native.c): the game's original code runs as it is, loaded at its
Xbox addresses, and only the Xbox libraries the port replaces are redirected.

Usage: python gen_native.py <decomp_dir> <llvm_mingw_bin_dir>

Writes build/native/native_tables.s:
  native_hooks[]   { Xbox address, port function }: a library function the port implements (its symbol names it,
                   D3D8__D3DDevice_RunPushBuffer or FUN_0012d671); native.c writes a jmp to the port function there
  native_kernel[]  { kernel import table slot, function }: port/kernel.c's xboxkrnl_<name> where it has one, else a stub
                   that reports the import
and, as absolute symbols in the same file, the game labels the port reads (DAT_, PTR_) at their Xbox addresses,
where the loaded image has them.

Not hooked: port_crt_math (x87 helpers such as _ftol take their argument in ST0, which a C function can't match; the
original runtime works natively), port_cxx_runtime (the same, for the C++ runtime), port_game_fixes (decompiler
repairs, meaningless for the original code) and port_startup (the recompiled build's entry point).
"""
import csv
import os
import re
import struct
import subprocess
import sys

PORT_OBJECTS = ["calls", "crash", "d3d8", "dsound", "gpu", "nv2a", "platform", "pushbuffer", "xapi"]
HOOKED = {"d3d8", "dsound", "platform", "xapi"}


def main():
    decomp, mingw_bin = sys.argv[1], sys.argv[2]
    root = os.path.dirname(os.path.abspath(decomp))
    build = os.path.join(decomp, "build")
    out_dir = os.path.join(build, "native")
    os.makedirs(out_dir, exist_ok=True)
    analysis = os.path.join(root, "analysis")

    # The symbols come from the Ghidra project (DumpDataSymbols.java -> analysis/data_symbols.tsv), which only exists
    # where the analysis pipeline has run. The few this build uses (the library functions the port hooks, the kernel
    # import slots' names) are kept in analysis/native_symbols.tsv, which is in the repository, so the native build
    # needs no Ghidra: when data_symbols.tsv is there, it's read and native_symbols.tsv rewritten from it.
    full = os.path.join(analysis, "data_symbols.tsv")
    kept = os.path.join(analysis, "native_symbols.tsv")
    source = full if os.path.exists(full) else kept
    names, labels = {}, {}
    for r in csv.DictReader(open(source), delimiter="\t"):
        if r["kind"] == "function":
            names[r["name"].replace("::", "__")] = int(r["address"], 16)
        elif r["kind"] == "label" and "." not in r["name"]:
            labels.setdefault(int(r["address"], 16), r["name"])
    used = []  # (name, address, kind) rows this build reads, for native_symbols.tsv

    nm = os.path.join(mingw_bin, "llvm-nm.exe")
    hooks, undefined, seen = [], set(), set()
    for name in PORT_OBJECTS:
        obj = os.path.join(out_dir, f"port_{name}.obj")  # tools/build_native.ps1 compiles them first
        for line in subprocess.run([nm, obj], capture_output=True, text=True).stdout.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == "U":
                undefined.add(parts[1])
                continue
            if len(parts) != 3 or parts[1] != "T" or name not in HOOKED:
                continue
            raw = parts[2]
            plain = re.sub(r"@\d+$", "", raw.lstrip("_@"))
            m = re.fullmatch(r"FUN_([0-9a-f]{8})", plain)
            address = int(m.group(1), 16) if m else names.get(plain)
            if address is not None and address not in seen:
                seen.add(address)
                hooks.append((address, raw, plain))
                if not m:
                    used.append((plain, address, "function"))

    # the kernel import table: one word per import (0x80000000 | ordinal) at the start of .rdata, ending with 0
    data = os.environ.get("DRA2_DATA") or os.path.join(root, "extracted")  # the launcher's game data folder
    xbe_path = os.path.join(data, "default.xbe")
    if not os.path.exists(xbe_path):
        # No game data (the release build, CI): the import slots are the kernel labels kept in native_symbols.tsv,
        # one per slot, contiguous from the start of .rdata, which is what the loop below reads from the game.
        if source == full:
            sys.exit(f"{xbe_path} is missing: extract the disc image first (or set DRA2_DATA to the extracted folder)")
        slots = sorted(labels)
        if not slots or slots[-1] - slots[0] != 4 * (len(slots) - 1):
            sys.exit(f"{kept}'s kernel labels aren't one contiguous import table: extract the disc image and rebuild")
        write_tables(out_dir, hooks, [(s, labels[s]) for s in slots], undefined, nm)
        return
    xbe = open(xbe_path, "rb").read()
    base = struct.unpack_from("<I", xbe, 0x104)[0]
    count, headers = struct.unpack_from("<II", xbe, 0x11c)
    rdata = None
    for k in range(count):
        flags, va, vsize, raw, rsize, name_at = struct.unpack_from("<IIIIII", xbe, headers - base + 56 * k)
        if xbe[name_at - base:name_at - base + 8].split(b"\0")[0] == b".rdata":
            rdata = (va, raw)
    kernel = []
    for k in range(1024):
        w, = struct.unpack_from("<I", xbe, rdata[1] + 4 * k)
        if not w & 0x80000000:
            break
        slot = rdata[0] + 4 * k
        kernel.append((slot, labels.get(slot, f"ordinal_{w & 0xffff}")))
        if slot in labels:
            used.append((labels[slot], slot, "label"))
    if source == full:
        with open(kept, "w", newline="") as f:
            f.write("name\taddress\tkind\n")
            for n, a, kind in sorted(used, key=lambda u: (u[1], u[0])):
                f.write(f"{n}\t{a:08x}\t{kind}\n")
    elif len(kernel) and not any(slot in labels for slot, _ in kernel):
        sys.exit(f"{kept} has no kernel import names: the game can't run without them")
    write_tables(out_dir, hooks, kernel, undefined, nm)


def write_tables(out_dir, hooks, kernel, undefined, nm):
    asm =["# Generated by tools/gen_native.py.", "\t.data", "\t.p2align 2",
           "\t.globl _native_hooks", "_native_hooks:"]
    asm += [f"\t.long 0x{a:08x}, {raw}  # {plain}" for a, raw, plain in sorted(hooks)]
    # the kernel exports port/kernel.c implements (xboxkrnl_<name>, any convention); the rest get reporting stubs
    implemented = {}
    kobj = os.path.join(out_dir, "kernel.obj")
    for line in subprocess.run([nm, kobj], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "TDB":
            m = re.fullmatch(r"[_@]?xboxkrnl_(\w+?)(?:@\d+)?", parts[2])
            if m:
                implemented[m.group(1)] = parts[2]
    asm += ["\t.long 0, 0", "\t.globl _native_kernel", "_native_kernel:"]
    asm += [f"\t.long 0x{slot:08x}, {implemented.get(name, f'Lkernel_{i}')}  # {name}"
            for i, (slot, name) in enumerate(kernel)]
    asm += ["\t.long 0, 0", "\t.text"]
    for i, (slot, name) in enumerate(kernel):
        asm += [f"Lkernel_{i}:", f"\tpushl $Lkname_{i}", "\tcalll _port_kernel_unimplemented", f"Lkname_{i}:",
                f'\t.asciz "{name}"']
    # game labels the port reads: absolute symbols at their Xbox addresses
    placed = 0
    for sym in sorted(undefined):
        m = re.fullmatch(r"_(?:_?DAT|PTR_\w*?)_([0-9a-f]{8})", sym)
        if m:
            asm += [f"\t.globl {sym}", f"\t.set {sym}, 0x{m.group(1)}"]
            placed += 1
    with open(os.path.join(out_dir, "native_tables.s"), "w") as f:
        f.write("\n".join(asm) + "\n")
    print(f"native tables: {len(hooks)} library functions hooked, {len(kernel)} kernel imports ({len(implemented)} "
          f"implemented), "
          f"{placed} game labels placed")


if __name__ == "__main__":
    main()
