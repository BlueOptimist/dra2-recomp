"""Spell out the integer <-> float "conversions" in a preprocessed file that are really bit copies, from clang's AST.

The decompiler prints a reinterpretation of 32 bits and a numeric conversion the same way: "(float)x", or an
assignment between an integer and a float variable. In C both convert the value, so the bits of 1.0f become
1065353216.0f. Two kinds of evidence decide which is meant:

 - Ghidra's untyped 32-bit values: an *implicit* conversion between undefined4 and float is a bit copy (Ghidra writes
   a numeric conversion as an explicit cast).
 - The machine code (analysis/fp_conversions.tsv, tools/find_fp_conversions.py): a function without fild/fist-type
   instructions (or SSE cvt*, or a call to _ftol) can't convert in that direction, so every integer <-> float
   conversion in it, implicit or a cast, is a bit copy. That covers "(float)*param_1" through a mistyped int pointer
   (the viewport rect) and integer-typed variables holding float bits.

They become ghidra_bits_float(x) / ghidra_float_bits(x) (ghidra_types.h); a cast's own text is removed.

clang's text AST dump prints each source location relative to the previous one ("line:12:5", or "col:9" on the same
line), so the dump is read in order with a location tracker.

Usage (as a module): rewrite(path, clang, flags, fp_table) -> number of conversions rewritten.
"""
import bisect
import re
import subprocess

LOC = re.compile(r"(?:<|, | )(?:line:(\d+):(\d+)|col:(\d+)|([A-Za-z]:\\[^:<>,]+?|[^\s<>:,]+?):(\d+):(\d+))")
FLOATS = r"'(?:float|float10|double)'(?::'(?:float|double)')?"
CAST = re.compile(r"(ImplicitCastExpr|CStyleCastExpr) 0x[0-9a-f]+ <([^>]*)> '([^']+)'(?::'([^']+)')? "
                  r"<(IntegralToFloating|FloatingToIntegral)>")
FLOAT_TYPES = {"float", "float10", "double"}
INT32_TYPES = {"undefined4", "int", "uint", "unsigned int", "long", "ulong", "unsigned long", "dword", "DWORD"}
TYPE = re.compile(r"'([^']+)'(?::'([^']+)')?")
THROUGH_POINTER = re.compile(r"UnaryOperator 0x[0-9a-f]+ <[^>]*> '[^']*'(?::'[^']*')? lvalue prefix '\*'|"
                             r"ArraySubscriptExpr 0x")
FUNC_HEADER = re.compile(r"^// ([0-9a-f]{8}) \S+ \(\d+ bytes\)", re.M)


def load_fp_table(path):
    """address -> (converts int to float, converts float to int, field offsets it loads with fild)"""
    table = {}
    try:
        for line in open(path).read().splitlines()[1:]:
            c = line.split("\t")
            fild = {int(x, 16) for x in c[4].split(",") if x} if len(c) > 4 else set()
            table[int(c[0], 16)] = (c[2] == "1", c[3] == "1", fild)
    except OSError:
        pass
    return table


FIELD = (re.compile(r"^\s*[A-Za-z_]\w*\s*\[\s*(0x[0-9a-f]+|\d+)\s*\]\s*$"),                      # p[6]: 4 * 6
         re.compile(r"^\s*\*\s*\(\s*\w+\s*\*\s*\)\s*\(\s*(?:\(int\)\s*)?[A-Za-z_]\w*\s*\+\s*(0x[0-9a-f]+|\d+)\s*\)\s*$"),
         re.compile(r"^\s*\*\s*[A-Za-z_]\w*\s*$"))                                                 # *p: 0


def field_offset(operand):
    """The byte offset of a field read "p[6]", "*(int *)(p + 0x18)", "*p"; None for anything else."""
    m = FIELD[0].match(operand)
    if m:
        return 4 * int(m.group(1), 0)
    m = FIELD[1].match(operand)
    if m:
        return int(m.group(1), 0)
    return 0 if FIELD[2].match(operand) else None


def token_end(text, line_starts, line, col):
    """Offset just past the token starting at (line, col): clang ranges end at the last token's start."""
    i = line_starts[line - 1] + col - 1
    if i < len(text) and (text[i].isalnum() or text[i] == "_"):
        while i < len(text) and (text[i].isalnum() or text[i] in "_."):
            i += 1
        return i
    return i + 1


def casts(dump_lines, main_file):
    """Every int <-> float cast in the main file: dicts with kind ('float' = to float, 'bits' = to an integer),
    explicit, the cast's range, the operand's start and type, whether the operand is read through a pointer."""
    found = []
    cur_file, cur_line = None, 0
    open_ = None  # the cast waiting for its operand lines
    for raw in dump_lines:
        positions = []
        for m in LOC.finditer(raw):
            if m.group(1):
                cur_line, col = int(m.group(1)), int(m.group(2))
            elif m.group(3):
                col = int(m.group(3))
            else:
                cur_file, cur_line, col = m.group(4), int(m.group(5)), int(m.group(6))
            positions.append((cur_file, cur_line, col))
        if open_ is not None:
            c = open_
            if c["operand_type"] is None:  # the operand (or its lvalue-to-rvalue read)
                t = TYPE.search(raw.split(">", 1)[-1] if ">" in raw else raw)
                c["operand_type"] = (t.group(2) or t.group(1)) if t else "?"
                c["operand_alias"] = t.group(1) if t else "?"
                c["operand_start"] = (positions[0][1], positions[0][2]) if positions else None
                if "<LValueToRValue>" not in raw:
                    c["through_pointer"] = bool(THROUGH_POINTER.search(raw))
                    found.append(c)
                    open_ = None
                continue
            c["through_pointer"] = bool(THROUGH_POINTER.search(raw))
            found.append(c)
            open_ = None
        m = CAST.search(raw)
        if m and positions:
            ends = positions[:2] if ", " in m.group(2) else positions[:1] * 2
            (f1, l1, c1), (f2, l2, c2) = ends
            if f1 and f1.endswith(main_file) and f2 == f1:
                open_ = {"explicit": m.group(1) == "CStyleCastExpr", "range": ((l1, c1), (l2, c2)),
                         "kind": "float" if m.group(5) == "IntegralToFloating" else "bits",
                         "target": m.group(4) or m.group(3), "target_alias": m.group(3), "operand_type": None}
    return found


def is_bit_copy(c, converts, operand=""):
    """converts: (int->float, float->int, fild field offsets) of the cast's function, or None if unknown."""
    if c["kind"] == "float":
        source = c["operand_alias"]
        if source not in INT32_TYPES and c["operand_type"] not in INT32_TYPES:
            return False
        if not c["explicit"] and source == "undefined4":
            return True
        if converts is None:
            return False
        if not converts[0]:
            return True
        # the function converts somewhere: a field it never loads with fild is still a float read
        offset = field_offset(operand) if c.get("through_pointer") else None
        return offset is not None and offset not in converts[2]
    target = c["target_alias"]
    if target not in INT32_TYPES and c["target"] not in INT32_TYPES:
        return False
    if c["operand_alias"] not in FLOAT_TYPES and c["operand_type"] not in FLOAT_TYPES:
        return False
    if not c["explicit"] and target == "undefined4":
        return True
    return converts is not None and not converts[1]


def rewrite(path, clang, flags, fp_table=None):
    text = open(path, encoding="latin-1").read()
    proc = subprocess.run([clang, *flags, "-Xclang", "-ast-dump", path], capture_output=True, text=True,
                          encoding="latin-1")
    main_file = path.replace("/", "\\").split("\\")[-1]
    found = casts(proc.stdout.splitlines(), main_file)
    if not found:
        return 0
    line_starts = [0]
    for i, ch in enumerate(text):
        if ch == "\n":
            line_starts.append(i + 1)
    headers = [(text.count("\n", 0, m.start()) + 1, int(m.group(1), 16)) for m in FUNC_HEADER.finditer(text)]
    header_lines = [h[0] for h in headers]

    def function_at(line):
        k = bisect.bisect_right(header_lines, line) - 1
        return headers[k][1] if k >= 0 else None

    inserts, removals, seen = [], [], set()
    for c in found:
        (l1, c1), (l2, c2) = c["range"]
        fn = function_at(l1)
        converts = fp_table.get(fn) if fp_table and fn is not None else None
        start = line_starts[l1 - 1] + c1 - 1
        end = token_end(text, line_starts, l2, c2)
        operand = ""
        if c.get("operand_start"):
            o = line_starts[c["operand_start"][0] - 1] + c["operand_start"][1] - 1
            if start <= o <= end:
                operand = text[o:end]
        if not is_bit_copy(c, converts, operand):
            continue
        if (start, end) in seen or end <= start:
            continue
        name = "ghidra_bits_float" if c["kind"] == "float" else "ghidra_float_bits"
        if c["explicit"]:  # "(float)x" -> "ghidra_bits_float(x)": the cast's text goes
            if not c["operand_start"]:
                continue
            operand = line_starts[c["operand_start"][0] - 1] + c["operand_start"][1] - 1
            if not (start < operand <= end) or not re.fullmatch(r"\(\s*[A-Za-z_][\w ]*\)\s*", text[start:operand]):
                continue
            removals.append((start, operand))
        seen.add((start, end))
        inserts.append((start, 1, -end, name + "("))
        inserts.append((end, 0, -start, ")"))
    # Applied from the end of the file. At one position: closing parentheses go before opening ones; of two ranges
    # opening there the outer one (ending later) must come first, of two closing there the inner one. Each insert
    # lands left of those applied before it, so the outer opening and the inner closing are applied last (the third
    # key). A cast's text is removed first (it lies between its opening insert and the operand).
    ops = [(pos, order, tie, s, 0) for pos, order, tie, s in inserts] + [(a, 2, 0, b, 1) for a, b in removals]
    for pos, order, tie, s, kind in sorted(ops, key=lambda t: (t[0], t[1], t[2]), reverse=True):
        if kind:
            text = text[:pos] + text[s:]
        else:
            text = text[:pos] + s + text[pos:]
    with open(path, "w", encoding="latin-1") as f:
        f.write(text)
    return len(seen)
