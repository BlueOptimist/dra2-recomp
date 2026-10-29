"""Compile the preprocessed decompiled code to real objects and report what a link would still need.

Usage: python build_objects.py <decomp_dir> <llvm_bin_dir> [--jobs=N] [--target=i686-w64-mingw32]

The default target is i686-pc-windows-msvc (Microsoft's CRT and SDK libraries). With llvm-mingw's bin folder and
--target=i686-w64-mingw32, the objects are built for MinGW-w64 instead, which links without Visual Studio.

Run tools/compile_check.py first: it writes build/pp/ (the compilable form of src/) and build/compile_report.tsv.
  1. copies build/pp/ to build/obj_src/, replacing the body of every function that still has compile errors with a
     trap, so each file compiles
  2. clang -c (32-bit Windows, -O0) every file into build/obj/
  3. llvm-nm over the objects: every symbol some object needs and none defines, sorted by kind:
       function in lua/crt/xapi (lib/ code that a port links or replaces), other undefined functions,
       data labels (DAT_/PTR_/s_ ...: the program's .data/.rdata, still to be emitted), vtables, other
Writes build/link_report.txt (summary + every symbol) and build/undefined_symbols.tsv (symbol, kind, used by files).
"""
import collections
import concurrent.futures
import csv
import glob
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compile_check import CLANG_FLAGS, FUNC_HEADER  # noqa: E402

OBJ_FLAGS = [f for f in CLANG_FLAGS if f != "-fsyntax-only"] + ["-c", "-O0", "-fno-builtin"]


def stub_functions(text, broken):
    """Replace the bodies of the functions whose addresses are in `broken` with a trap."""
    lines = text.split("\n")
    out = []
    i = 0
    stubbed = 0
    while i < len(lines):
        m = FUNC_HEADER.match(lines[i])
        if m and m.group(1) in broken:
            # signature lines up to the opening brace at column 0, then skip to the closing brace at column 0.
            # The stub is weak: a hand-written replacement in decomp/port/ takes over at link time.
            out.append(lines[i])
            i += 1
            first_sig = True
            while i < len(lines) and lines[i] != "{":
                line = lines[i]
                if first_sig and line.strip() and not line.strip().startswith(("/*", "*", "//")):
                    line = "__attribute__((weak)) " + line
                    first_sig = False
                out.append(line)
                i += 1
            out.append("{")
            out.append(f"  __builtin_trap(); /* {m.group(1)}: decompiled body does not compile yet */")
            i += 1
            while i < len(lines) and lines[i] != "}":
                i += 1
            out.append("}")
            stubbed += 1
            i += 1
            continue
        out.append(lines[i])
        i += 1
    return "\n".join(out), stubbed


TARGET = "i686-pc-windows-msvc"


def compile_obj(args):
    clang, src, obj, includes, *extra = args
    flags = [f"--target={TARGET}" if f.startswith("--target=") else f for f in OBJ_FLAGS] + list(extra)
    proc = subprocess.run([clang, *flags, *[f"-I{i}" for i in includes], src, "-o", obj],
                          capture_output=True, text=True, encoding="latin-1")
    return src, proc.returncode, proc.stderr


def nm(llvm_nm, obj):
    proc = subprocess.run([llvm_nm, "--format=posix", obj], capture_output=True, text=True, encoding="latin-1")
    defined, undefined = set(), set()
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        name, kind = parts[0], parts[1]
        name = name[1:] if name.startswith("_") and not name.startswith("__") else name  # i686 C decoration
        name = re.sub(r"@\d+$", "", name)  # __stdcall/__fastcall decoration
        name = name.lstrip("@")
        if kind == "U":
            undefined.add(name)
        elif kind not in ("w", "v"):
            defined.add(name)
    return defined, undefined


def main():
    global TARGET
    decomp, llvm_bin = sys.argv[1], sys.argv[2]
    jobs = next((int(a.split("=")[1]) for a in sys.argv[3:] if a.startswith("--jobs=")), os.cpu_count())
    TARGET = next((a.split("=", 1)[1] for a in sys.argv[3:] if a.startswith("--target=")), TARGET)
    # --instrument: the game's functions count their calls (port/calls.c; DRA2_SAMPLE prints what ran)
    instrument = ["-finstrument-functions"] if "--instrument" in sys.argv[3:] else []
    clang = os.path.join(llvm_bin, "clang.exe")
    llvm_nm = os.path.join(llvm_bin, "llvm-nm.exe")
    build = os.path.join(decomp, "build")
    pp, obj_src, obj_dir = (os.path.join(build, d) for d in ("pp", "obj_src", "obj"))

    broken = {r["address"] for r in csv.DictReader(open(os.path.join(build, "compile_report.tsv")), delimiter="\t")
              if r["errors"] != "0"}
    # game functions port/ rewrites by hand ("/* replaces 00156710 ... */"): their decompiled body becomes a weak
    # stub too, so the port's definition wins at link time
    replaced = set()
    for src in glob.glob(os.path.join(decomp, "port", "*.c")):
        replaced |= set(re.findall(r"replaces ([0-9a-f]{8})\b", open(src, encoding="latin-1").read()))
    broken |= replaced
    if replaced:
        print(f"functions replaced by port/: {len(replaced)}")
    for d in (obj_src, obj_dir):
        shutil.rmtree(d, ignore_errors=True)
    jobs_list = []
    stubbed = 0
    src_root = os.path.join(decomp, "src")
    for dp, _, fns in os.walk(src_root):  # only files that exist in src/ now (pp/ can hold older exports)
        for fn in sorted(fns):
            if not fn.endswith(".c"):
                continue
            rel = os.path.relpath(os.path.join(dp, fn), src_root)
            pp_file = os.path.join(pp, rel)
            if not os.path.exists(pp_file):
                continue
            text, n = stub_functions(open(pp_file, encoding="latin-1").read(), broken)
            stubbed += n
            src = os.path.join(obj_src, rel)
            os.makedirs(os.path.dirname(src), exist_ok=True)
            with open(src, "w", encoding="latin-1") as f:
                f.write(text)
            obj = os.path.join(obj_dir, rel[:-2].replace(os.sep, "_") + ".obj")
            os.makedirs(obj_dir, exist_ok=True)
            jobs_list.append((clang, src, obj, [build, decomp], *instrument))
    print(f"functions stubbed (still have compile errors): {stubbed}")
    # hand-written port code (runtime stand-ins, later the platform layer)
    for src in sorted(glob.glob(os.path.join(decomp, "port", "*.c"))):
        jobs_list.append((clang, src, os.path.join(obj_dir, "port_" + os.path.basename(src)[:-2] + ".obj"),
                          [build, decomp]))

    # Lua 5.0.3 (third_party/), built with float numbers like the game's copy: its own flags, no decompiler ones
    lua = os.path.join(decomp, "..", "third_party", "lua-5.0.3")
    lua_flags = [f"--target={TARGET}", "-c", "-O2", "-w", "-DLUA_NUMBER=float", '-DLUA_NUMBER_SCAN="%f"',
                 '-DLUA_NUMBER_FMT="%.7g"', "-I" + os.path.join(lua, "include"), "-I" + os.path.join(lua, "src")]
    lua_failed = []
    lua_sources = [s for s in sorted(glob.glob(os.path.join(lua, "src", "*.c"))) if not s.endswith("ltests.c")]
    lua_sources += sorted(glob.glob(os.path.join(lua, "src", "lib", "*.c")))
    for src in lua_sources:
        obj = os.path.join(obj_dir, "lua_" + os.path.basename(src)[:-2] + ".obj")
        proc = subprocess.run([clang, *lua_flags, src, "-o", obj], capture_output=True, text=True, encoding="latin-1")
        if proc.returncode:
            lua_failed.append((src, proc.stderr.strip().splitlines()[0] if proc.stderr.strip() else "?"))
    print(f"Lua 5.0.3: {len(lua_sources) - len(lua_failed)} of {len(lua_sources)} files compiled (LUA_NUMBER float)")
    for src, err in lua_failed:
        print(f"  FAILED {os.path.basename(src)}: {err}")

    failed = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for src, rc, err in pool.map(compile_obj, jobs_list):
            if rc != 0:
                failed.append((src, err))
    print(f"objects: {len(jobs_list) - len(failed)} of {len(jobs_list)} files compiled")
    for src, err in failed:
        first = next((l for l in err.splitlines() if "error" in l), err[:200])
        print(f"  FAILED {os.path.relpath(src, build)}: {first}")

    # symbols
    defined_by, needed_by = {}, collections.defaultdict(set)
    for _, src, obj, *_ in jobs_list:
        if not os.path.exists(obj):
            continue
        d, u = nm(llvm_nm, obj)
        for s in d:
            defined_by.setdefault(s, os.path.basename(obj))
        for s in u:
            needed_by[s].add(os.path.basename(obj))
    missing = {s: files for s, files in needed_by.items() if s not in defined_by}

    modules = [(int(r["start"], 16), int(r["end"], 16), r["module"])
               for r in csv.DictReader(open(os.path.join(decomp, "..", "analysis", "modules.tsv")), delimiter="\t")]

    def kind(sym):
        m = re.search(r"(?:FUN|thunk_FUN|LAB)_([0-9a-f]{8})$", sym) or re.search(r"_([0-9a-f]{8})$", sym)
        if sym.startswith(("DAT_", "_DAT_", "PTR_", "s_", "u_", "UNK_", "luakey_", "stack0x", "register0x")) or \
                re.match(r"[a-z]{1,4}Ram[0-9a-f]{8}$", sym) or re.match(r"_(?:param|local)_", sym) or \
                re.match(r"_[a-zA-Z]{1,4}Stack_", sym):
            return "data label"
        if sym.endswith("_vftable_data"):
            return "vtable data"
        if sym.startswith(("XAPILIB__", "D3D8__", "XBE_FILE_HEADER_")) or sym.startswith("Xbox") or sym.startswith("Xe"):
            return "xbox system"
        if m and sym.startswith(("FUN_", "thunk_FUN_", "LAB_")):
            addr = int(m.group(1), 16)
            for start, end, mod in modules:
                if start <= addr < end:
                    return f"function in {mod}"
            return "function (game, not in src/)"
        if sym.startswith(("LAB_", "switchD_", "caseD_", "code_r0x")):
            return "code label"
        if sym.startswith("__") or sym in ("memcpy", "memset", "strlen", "strcmp", "_fltused"):
            return "compiler/CRT helper"
        return "named function or label"

    by_kind = collections.defaultdict(list)
    for s in sorted(missing):
        by_kind[kind(s)].append(s)

    with open(os.path.join(build, "undefined_symbols.tsv"), "w", newline="") as f:
        w = csv.writer(f, delimiter="\t")
        w.writerow(["symbol", "kind", "files"])
        for k, syms in sorted(by_kind.items()):
            for s in syms:
                w.writerow([s, k, len(missing[s])])
    with open(os.path.join(build, "link_report.txt"), "w") as f:
        f.write(f"objects: {len(jobs_list) - len(failed)}/{len(jobs_list)}, defined symbols: {len(defined_by)}, "
                f"undefined: {len(missing)}\n\n")
        for k, syms in sorted(by_kind.items(), key=lambda kv: -len(kv[1])):
            f.write(f"== {k}: {len(syms)}\n")
            for s in syms:
                f.write(f"  {s}  ({len(missing[s])} files)\n")
    print(f"defined symbols: {len(defined_by)}, undefined: {len(missing)}")
    for k, syms in sorted(by_kind.items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(syms):6}  {k}   e.g. {', '.join(syms[:5])}")


if __name__ == "__main__":
    main()
