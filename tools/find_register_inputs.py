"""Find functions whose decompiled body reads a register their calling convention doesn't pass.

Usage: python find_register_inputs.py <decomp_dir> <ecx_this.tsv>

Ghidra writes such inputs as in_ECX / in_EAX / in_EDX; recompiled, they are uninitialised locals. Prints a summary per
register and convention, and writes the stack-convention functions that read ECX (all __thiscall methods Ghidra
missed) to the TSV for SetThiscall.java. Fixing one round can expose more: a caller of a newly __thiscall function
now shows the in_ECX it passes on, so run export -> this -> SetThiscall until the list is empty.
"""
import collections
import glob
import os
import re
import sys


def main():
    decomp, out = sys.argv[1], sys.argv[2]
    fn_regs, conv = collections.defaultdict(set), {}
    for p in glob.glob(os.path.join(decomp, "src", "**", "*.c"), recursive=True):
        parts = re.split(r"\n// ([0-9a-f]{8}) (\S+) \((\d+) bytes\)\n", open(p, encoding="latin-1").read())
        for i in range(1, len(parts) - 3, 4):
            addr, body = parts[i], parts[i + 3]
            sig = body.split("{", 1)[0]
            conv[addr] = "thiscall" if "__thiscall" in sig else "fastcall" if "__fastcall" in sig else "stack"
            for r in re.findall(r"\bin_(EAX|ECX|EDX|EBX|ESI|EDI)\b", body):
                fn_regs[addr].add(r)
    summary = collections.Counter((r, conv[a]) for a, rs in fn_regs.items() for r in rs)
    for (reg, c), n in summary.most_common():
        print(f"  {n:5}  in_{reg} in a {c} function")
    ecx = sorted(a for a, rs in fn_regs.items() if "ECX" in rs and conv[a] == "stack")
    with open(out, "w") as f:
        f.write("address\n" + "".join(a + "\n" for a in ecx))
    print(f"stack-convention functions reading ECX (-> __thiscall): {len(ecx)}")
    # EAX/EDX inputs: no standard convention passes them; AddRegisterParams.java adds them as explicit parameters
    regs_out = os.path.join(os.path.dirname(out), "register_params.tsv")
    rows = sorted((a, ",".join(r for r in ("EAX", "EDX") if r in rs)) for a, rs in fn_regs.items()
                  if {"EAX", "EDX"} & rs)
    with open(regs_out, "w") as f:
        f.write("address\tregisters\n" + "".join(f"{a}\t{r}\n" for a, r in rows))
    print(f"functions reading EAX/EDX as inputs (-> explicit register parameters): {len(rows)}")


if __name__ == "__main__":
    main()
