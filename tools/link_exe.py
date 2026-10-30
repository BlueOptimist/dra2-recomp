"""Link the objects in decomp/build/obj into decomp/build/dra2.exe with llvm-mingw (no Visual Studio needed).

Usage: python link_exe.py <decomp_dir> <llvm_mingw_bin_dir>

Run after build_objects.py --target=i686-w64-mingw32, gen_platform_stubs.py and gen_data.py --target=... .
The image is linked at 0x400000 without ASLR (port/startup.c tells unrelocated Xbox addresses apart by range).
If symbols are still missing the link fails; every undefined symbol is listed in build/link_errors.txt, with
the objects that need it, and summarised by kind on the console.
"""
import collections
import glob
import os
import re
import subprocess
import sys


def main():
    decomp, mingw_bin = sys.argv[1], sys.argv[2]
    build = os.path.join(decomp, "build")
    objs = sorted(glob.glob(os.path.join(build, "obj", "*.obj")))
    exe = os.path.join(build, "dra2.exe")
    cmd = [os.path.join(mingw_bin, "i686-w64-mingw32-clang.exe"), "-o", exe, *objs,
           "-Wl,--image-base=0x400000", "-Wl,--disable-dynamicbase", "-Wl,--error-limit=0", "-lm",
           "-luser32", "-lgdi32", "-ld3d9"]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="latin-1")
    out = proc.stdout + proc.stderr
    with open(os.path.join(build, "link_errors.txt"), "w", encoding="latin-1") as f:
        f.write(out)
    if proc.returncode == 0:
        print(f"linked {exe} ({os.path.getsize(exe) // 1024} KB) from {len(objs)} objects")
        return
    undefined = collections.defaultdict(set)
    for m in re.finditer(r"undefined symbol: (\S+)\s*\n(?:>>> referenced by (\S+))?", out):
        undefined[m.group(1)].add(m.group(2) or "?")
    kinds = collections.Counter()
    for sym in undefined:
        s = sym.lstrip("_")
        if re.match(r"(?:thunk_)?(?:FUN|LAB)_", s):
            kinds["code not compiled (FUN_/LAB_)"] += 1
        elif re.match(r"(?:DAT|PTR|UNK|s|u)_", s):
            kinds["data label"] += 1
        elif re.search(r"Ram[0-9a-f]{8}$|^(?:param|local)_|Stack_|^stack0x|^register0x", s):
            kinds["decompiler pseudo-variable"] += 1
        elif re.match(r"lua", s):
            kinds["Lua 5.0"] += 1
        else:
            kinds["other"] += 1
    print(f"link failed: {len(undefined)} undefined symbols (full list: build/link_errors.txt)")
    for k, v in kinds.most_common():
        print(f"  {v:6}  {k}")
    other = sorted(s for s in undefined if not re.match(r"_*(?:thunk_)?(?:FUN|LAB|DAT|PTR|UNK|s|u)_|.*Ram[0-9a-f]{8}$", s))
    print("  e.g. " + " ".join(other[:40]))
    other_errors = [l for l in out.splitlines() if "error" in l and "undefined symbol" not in l]
    if other_errors:
        print("other errors:")
        for l in other_errors[:15]:
            print("  " + l)


if __name__ == "__main__":
    main()
