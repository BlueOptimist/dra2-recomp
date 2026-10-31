"""Which functions really convert between integers and floats: analysis/fp_conversions.tsv.

Usage: python find_fp_conversions.py <text.dis> <decomp_dir> <out.tsv> [text base, default 11000]

The decompiler prints a reinterpretation of 32 bits and a numeric conversion the same way, "(float)x" or an implicit
assignment between an int and a float, so the C source alone can't tell "load a float through an int pointer" from
"convert this integer" (FUN_0019ebb0 read a viewport rect of floats through an undefined4 pointer; the texture
matrix's 1.0 became 1065353216.0). The machine code can: int -> float needs fild/fiadd/fisub/fimul/fidiv/ficom or an
SSE cvtsi2ss/cvtdq2ps; a float -> int printed as a plain cast needs an SSE cvt(t)ss2si/cvt(t)ps2dq (see TO_INT).
A function with none of them only moves bits. tools/implicit_bits.py rewrites the conversions in such functions as bit
copies.

Output columns: address, name, int_to_float (0/1), float_to_int (0/1), and the register-relative field offsets the
function loads with fild and with fld (dword): in a function that does convert somewhere, "(float)param_2[6]" is still
a float read when +0x18 is loaded with fld and never with fild.
"""
import bisect
import glob
import os
import re
import sys

INSN = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*?)\s*$")
TO_FLOAT = ("fild", "fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr", "ficom", "ficomp", "cvtsi2ss",
            "cvtsi2sd", "cvtdq2ps", "cvtpi2ps")
# float -> int as the decompiler prints a plain cast "(int)x": the SSE conversions. An x87 fist/fistp prints as
# "(int)ROUND(x)" and _ftol as a call, so a plain "(uint)local_12c" in a function with only those is a bit copy
# (FUN_001a9fd0 stored its particle vertices as integers).
TO_INT = ("cvtss2si", "cvttss2si", "cvtsd2si", "cvttsd2si", "cvtps2dq", "cvttps2dq", "cvtps2pi", "cvttps2pi")


def main():
    dis, decomp, out = sys.argv[1], sys.argv[2], sys.argv[3]
    base = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x11000
    funcs = []
    for path in glob.glob(os.path.join(decomp, "src", "**", "*.c"), recursive=True):
        for addr, name, size in re.findall(r"^// ([0-9a-f]{8}) (\S+) \((\d+) bytes\)$",
                                           open(path, encoding="latin-1").read(), re.M):
            funcs.append((int(addr, 16), int(size), name))
    funcs.sort()
    flags = {}
    starts = [f[0] for f in funcs]
    for line in open(dis, encoding="latin-1"):
        m = INSN.match(line)
        if not m:
            continue
        op = m.group(2)  # AT&T spelling: fildl, fistpl, cvttss2si...
        to_float = op.startswith(TO_FLOAT)
        to_int = op.startswith(TO_INT)
        loads_float = op == "flds"  # a 4-byte float load: which fields a function reads as floats
        if not (to_float or to_int or loads_float):
            continue
        addr = base + int(m.group(1), 16)
        k = bisect.bisect_right(starts, addr) - 1
        if k < 0 or addr >= funcs[k][0] + funcs[k][1]:
            continue
        f = flags.setdefault(funcs[k][0], [0, 0, set(), set()])
        f[0] |= to_float
        f[1] |= to_int
        # the displacement of a register-relative memory operand, "0x18(%eax)": a field the instruction reads
        # (stack slots, %esp/%ebp-relative, are locals and arguments, not fields)
        disp = re.match(r"(-?0x[0-9a-f]+|)\(%e(?:ax|bx|cx|dx|si|di)\)", m.group(3))
        if disp and op in ("fildl", "flds"):
            (f[2] if op == "fildl" else f[3]).add(int(disp.group(1) or "0", 16))
    with open(out, "w") as f:
        f.write("address\tname\tint_to_float\tfloat_to_int\tfild_fields\tfld_fields\n")
        for addr, size, name in funcs:
            a, b, fild, fld = flags.get(addr, [0, 0, set(), set()])
            f.write(f"{addr:08x}\t{name}\t{int(a)}\t{int(b)}\t{','.join(f'{d:x}' for d in sorted(fild))}\t"
                    f"{','.join(f'{d:x}' for d in sorted(fld))}\n")
    print(f"functions: {len(funcs)}, with int->float: {sum(1 for v in flags.values() if v[0])}, "
          f"with float->int: {sum(1 for v in flags.values() if v[1])} -> {out}")


if __name__ == "__main__":
    main()
