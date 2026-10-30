"""Find indirect calls whose callee pops its arguments, for per-call-site overrides.

Usage: python find_indirect_pops.py <text.dis> <decomp_dir> <out.tsv> [text base, default 11000]

The decompiler doesn't know what a call through a vtable (call [eax+10h]) calls, so it assumes the default cdecl:
the caller cleans up. The game's virtual methods are __thiscall and pop their own arguments, so after each such call
the decompiler's stack offsets are off by the arguments' size. A struct passed by address then gets two names, or a
parameter is read from the wrong slot (FUN_0013eab0 passed param_1 to the stream reader instead of param_3, 8 bytes
off after two allocator calls).
The machine code says which: arguments pushed before the call and no "add esp, N" after it means the callee popped
them. Writes call address, convention (__thiscall), return type, parameters ("pointer" for this, then one undefined4
per pushed argument), and a note, in the analysis/call_overrides.tsv format (ApplyCallOverrides.java).
"""
import glob
import os
import re
import sys

INSN = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*?)\s*$")
SAVED = ("%ebp", "%ebx", "%esi", "%edi")


def main():
    dis, decomp, out = sys.argv[1], sys.argv[2], sys.argv[3]
    base = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x11000
    insns = []
    for line in open(dis, encoding="latin-1"):
        m = INSN.match(line)
        if m and not m.group(2).startswith("<"):
            insns.append((base + int(m.group(1), 16), m.group(2), m.group(3)))
    index = {a: k for k, (a, _, _) in enumerate(insns)}
    # register saves: pushes of callee-saved registers among a function's first instructions, before its first call
    # (an early-exit branch may come before them). They are never arguments, and arguments can't come before them.
    saves = set()
    starts = set()  # instruction indexes of function entries
    for path in glob.glob(os.path.join(decomp, "src", "**", "*.c"), recursive=True):
        for start in re.findall(r"^// ([0-9a-f]{8}) \S+ \(\d+ bytes\)$", open(path, encoding="latin-1").read(), re.M):
            k = index.get(int(start, 16))
            if k is None:
                continue
            starts.add(k)
            for j in range(k, min(k + 32, len(insns))):
                op, args = insns[j][1], insns[j][2]
                if op == "calll":
                    break  # (an early-exit path with its own ret may come before the saves)
                if op == "pushl" and args in SAVED:
                    saves.add(j)
    # MSVC's exception-frame prologue: push -1; push handler; push fs:[0]; mov fs:[0], esp; then "push ecx" or
    # "sub esp, N" for the locals. None of it is an argument (FUN_0016ff70's allocator call was given five).
    for j, (_, op, args) in enumerate(insns):
        if op == "movl" and args == "%esp, %fs:0x0" and j + 1 < len(insns) and insns[j + 1][1] in ("pushl", "subl"):
            saves.add(j + 1)
    rows = []
    for k, (addr, op, args) in enumerate(insns):
        if op != "calll" or not args.startswith("*") or "%esp" in args:
            continue
        if not re.match(r"\*(?:-?0x[0-9a-f]+)?\(%e[a-d]x|\*(?:-?0x[0-9a-f]+)?\(%e[sd]i|\*(?:-?0x[0-9a-f]+)?\(%ebp", args):
            continue  # only calls through memory (vtables, function-pointer fields), not through a register
        # arguments: the run of pushes right before the call. Only the object/target setup (mov, lea) may sit between
        # them; anything else ends the run, so pushes for an outer call or saved registers aren't counted (an
        # argument count that is too high is as harmful as a missing one)
        # A struct or vector passed by value is not pushed: "sub esp, 0Ch" reserves its dwords and stores fill them
        # (MenuLegal::Setup passes a position as three floats that way), so a small sub esp in the run counts too.
        pushes = 0
        j = k - 1
        while j >= 0 and k - j <= 20:
            jop, jargs = insns[j][1], insns[j][2]
            if "%fs:" in jargs or j in saves and jop == "subl":
                break  # the exception-frame setup (above)
            if jop == "pushl":
                if j in saves:
                    break  # the function's own register saves (prologue)
                pushes += 1
            elif jop == "subl" and jargs.endswith(", %esp"):
                m = re.match(r"\$0x([0-9a-f]+), %esp$", jargs)
                if not m or int(m.group(1), 16) > 0x40 or int(m.group(1), 16) % 4 or \
                        any(s in starts for s in range(j - 3, j + 1)) or \
                        any(insns[i][1] in ("nop", "retl", "int3") for i in range(max(0, j - 3), j)):
                    break  # the function's own frame (prologue, maybe of a start Ghidra doesn't list), or not args
                # "sub esp, 0Ch; mov eax, esp; push eax; call": the area's address is this call's argument (it
                # returns a vector there, as the by-value argument of an outer call), so it isn't this call's
                copies = {a.split(", ")[1] for _, o, a in insns[j + 1:k]
                          if (o == "movl" and a.startswith("%esp, %")) or (o == "leal" and "(%esp), %" in a)}
                if any(o == "pushl" and a in copies for _, o, a in insns[j + 1:k]):
                    break
                pushes += int(m.group(1), 16) // 4
            elif jop not in ("movl", "leal", "movzbl", "movsbl", "movzwl", "movswl"):
                break  # (float stores end the run too: past them, "push esi; push edi" can be MSVC's late register
                # saves, which a run through x87 code took for two arguments in GameObject::UpdateFacing)
            j -= 1
        if not pushes or k + 1 >= len(insns):
            continue
        nop, nargs = insns[k + 1][1], insns[k + 1][2]
        if nop == "addl" and nargs.endswith(", %esp"):
            continue  # the caller cleans up: cdecl, which the decompiler already assumes
        params = ",".join(["pointer"] + ["undefined4"] * pushes)  # undefined4: a pushed float stays a bit copy
        rows.append((f"{addr:08x}", "__thiscall", "int", params, f"call through memory, {pushes} argument(s) pushed, "
                     f"no add esp after: the callee pops them"))
    with open(out, "w") as f:
        f.write("call\tconvention\treturn\tparams\tnote\n")
        for r in rows:
            f.write("\t".join(r) + "\n")
    print(f"indirect calls whose callee pops its arguments: {len(rows)} -> {out}")


if __name__ == "__main__":
    main()
