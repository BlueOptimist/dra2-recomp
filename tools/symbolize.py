"""Turn the addresses in a crash report (port/crash.c's "[crash] frame XXXXXXXX" lines) into function names.

Usage: python symbolize.py <dra2.exe> <llvm-nm.exe> [log file]   (reads stdin without a log file)

For each frame: the function containing the address, the offset into it, and the original Xbox address when the
name carries one (FUN_00143670 -> 0x143670), so the frame can be looked up in decomp/src or Ghidra.
"""
import bisect
import re
import subprocess
import sys


def main():
    exe, nm = sys.argv[1], sys.argv[2]
    text = open(sys.argv[3], encoding="latin-1").read() if len(sys.argv) > 3 else sys.stdin.read()
    out = subprocess.run([nm, "-n", "--defined-only", exe], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tT":
            addrs.append(int(parts[0], 16))
            names.append(re.sub(r"@\d+$", "", parts[2].lstrip("_")))
    for line in text.splitlines():
        m = re.match(r"\[crash\] (?:frame|stack) ([0-9a-fA-F]{8})", line)
        if m and line.startswith("[crash] stack"):
            a = int(m.group(1), 16)
            i = bisect.bisect_right(addrs, a) - 1
            # stack words: only those that look like code addresses in the .exe are worth naming
            if 0 <= i and addrs[0] <= a < addrs[-1] + 0x10000:
                print(f"  stack {a:08x}  {names[i]}+{a - addrs[i]:#x}")
            continue
        if not m:
            if line.startswith("[crash]") or line.startswith("[game]") or line.startswith("[port]"):
                print(line)
            continue
        a = int(m.group(1), 16)
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0:
            print(f"  {a:08x}  ?")
            continue
        name = names[i]
        xbox = re.search(r"_([0-9a-f]{8})$", name)
        print(f"  {a:08x}  {name}+{a - addrs[i]:#x}" + (f"   (Xbox {int(xbox.group(1), 16):#x})" if xbox else ""))


if __name__ == "__main__":
    main()
