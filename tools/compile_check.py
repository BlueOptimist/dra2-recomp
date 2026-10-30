"""Compile-check the decompiled game code with clang and record per-function status.

Usage: python compile_check.py <decomp_dir> <clang.exe> [--jobs=N]

For every decomp/src/**/*.c file:
  1. preprocess into decomp/build/pp/: Class::method -> Class__method, include build/program_types.h
     (all program types + prototypes of functions NOT defined in src/, from ExportHeader.java)
  2. prototypes for functions defined in src/ are taken from their exported definitions (build/protos.h), so a
     declaration always matches its definition; the database signatures often differ from what the decompiler
     emitted (e.g. no committed parameters)
  3. declare the global labels the code uses (DAT_*, PTR_*, LAB_*, s_*, stack0x*, ...) in build/globals.h
  3. clang -fsyntax-only as 32-bit Windows C (i686-pc-windows-msvc), C conversions that the raw output relies on
     downgraded to warnings, every error attributed to the function whose block it falls in
Writes decomp/build/compile_report.tsv (address, name, errors, first error) and updates the status column of
decomp/functions.tsv for game/engine code: 'compiles' or 'errors'. Also checks the format struct sizes.
"""
import collections
import concurrent.futures
import csv
import os
import re
import shutil
import subprocess
import sys

QUALIFIED = re.compile(r"\b([A-Za-z_]\w*)::(~?[A-Za-z_]\w*)")
FUNC_HEADER = re.compile(r"^// ([0-9a-f]{8}) (\S+) \(\d+ bytes\)$")
GLOBAL_ID = re.compile(r"\b((?:DAT|PTR|LAB|s|u|switchD|caseD|UNK|BYTE|WORD|DWORD|QWORD|FLOAT|DOUBLE|TEXT|"
                       r"PTR_LAB|PTR_FUN|PTR_DAT|PTR_s|PTR_PTR|PTR_u|ADDR|OFF|SUB|EXT|thunk_DAT|code_r0x)_[0-9A-Za-z_]+"
                       r"|_DAT_[0-9a-f]+|stack0x[0-9a-f]+|register0x[0-9a-f]+|[a-z]{1,4}Ram[0-9a-f]{8}"
                       r"|luakey_\w+|_param_\d+|_local_[0-9a-f]+|_[a-zA-Z]{1,4}Stack_[0-9a-f]+|XAPILIB__\w+"
                       r"|D3D8__D3DRS_\w+|XBE_FILE_HEADER_[0-9a-f]+)\b")
# Ghidra's names for memory at absolute addresses carry their type as a Hungarian prefix (piRam = int *).
RAM_PREFIX = {"i": "int", "u": "uint", "c": "char", "b": "byte", "f": "float", "s": "short", "us": "ushort",
              "uc": "uchar", "d": "double", "l": "int", "ul": "uint", "ll": "longlong", "v": "void", "w": "ushort"}


def ram_type(name):
    prefix = name[:name.index("Ram")]
    stars = 0
    while prefix.startswith("p") and prefix != "p":
        stars += 1
        prefix = prefix[1:]
    base = RAM_PREFIX.get(prefix, "undefined4")
    if base == "void" and stars == 0:
        base = "undefined4"
    return base + " " + "*" * stars


# switch on a float or pointer value (decompiler artefact: jump tables are integer code)
SWITCH_LINE = re.compile(r"^(\s*)switch\((.*)\)\s*\{\s*$")
CASE_POINTER = re.compile(r"\bcase \([A-Za-z_][\w ]*\*+\)\s*(0x[0-9a-fA-F]+|\d+):")
CASE_FLOAT = re.compile(r"\bcase (-?\d+)\.0+:")
# an integer case value printed through a float: n is shown as the denormal n * 2^-149 (1 -> 1.4013e-45)
CASE_DENORMAL = re.compile(r"\bcase (\d+(?:\.\d+)?e-4[0-9]):")
PROTO_NAME = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
# '&X_vftable' is the vtable's data, but X_vftable is also its struct type; C can't have both names.
VTABLE_REF = re.compile(r"&(\w+_vftable)\b")
# Ghidra's partial-variable syntax: "x._2_1_" is 1 byte at offset 2 of x (little-endian). Not valid C, so it becomes
# the equivalent access through a pointer: *(T *)((char *)&x + 2).
PARTIAL = re.compile(r"\b([A-Za-z_]\w*(?:(?:->|\.)(?!_\d+_\d+_)[A-Za-z_]\w*|\[[^\[\]]*\])*)\._(\d+)_(\d+)_\b")
PARTIAL_TYPES = {"1": "unsigned char", "2": "unsigned short", "3": "unsigned int", "4": "unsigned int",
                 "8": "unsigned long long"}
# A global is declared as a pointer when the code dereferences, subscripts or member-accesses it.
_G = r"((?:_?DAT|PTR)_\w+|_param_\d+|_local_[0-9a-f]+|_[a-zA-Z]{1,4}Stack_[0-9a-f]+)"
WIDE_TYPES = r"undefined8|longlong|ulonglong|double|float8"
GLOBAL_POINTER_USE = re.compile(rf"\*\s*\(?\s*{_G}\b|\b{_G}\s*\[|\b{_G}\s*->")
# Ghidra writes a 10-byte x87 return value as an array type ("undefined1 [10] f(...)"); C can't return arrays.
ARRAY10_RETURN = re.compile(r"^(\s*)undefined1\s+\[10\]\s+")
BARE_NAME_LINE = re.compile(r"(?m)^[A-Za-z_]\w*\s*$")
# Ghidra casts between floats and pointers are bit reinterpretations (one stack slot reused for both). C needs them
# spelled out: (float)ptr -> (*(float *)&ptr), (T *)floatvar -> (*(T **)&floatvar).
DECL = re.compile(r"^\s+([A-Za-z_][\w ]*?)\s*(\*+)?\s*([A-Za-z_]\w*)(\s*\[[^\]]*\])?;\s*$")
FLOAT_TYPES = {"float", "double", "float10"}
# (a cast followed by [ or -> applies to the element/member, not to the variable itself: leave those alone)
CAST_TO_FLOAT = re.compile(r"\((float|double|float10)\)\s*([A-Za-z_]\w*)\b(?!\s*[\(\[]|\s*->)")
CAST_TO_POINTER = re.compile(r"\(([A-Za-z_]\w*\s*\*+)\)\s*(?:\((?:float|double|float10)\)\s*)?([A-Za-z_]\w*)\b(?!\s*[\(\[]|\s*->)")


def fix_float_pointer_casts(lines):
    """Rewrite float<->pointer casts inside each function, using its own declarations."""
    out, kinds = [], {}
    for line in lines:
        if FUNC_HEADER.match(line.rstrip("\n")):
            kinds = {}
        m = DECL.match(line)
        if m and not line.lstrip().startswith("return"):
            base, stars, name = m.group(1).split()[-1], m.group(2), m.group(3)
            kinds[name] = "ptr" if stars else ("float" if base in FLOAT_TYPES else "other")
        elif "(" in line and line.rstrip().endswith(")") and not line.startswith(" "):
            # function signature: record parameter kinds
            for p in re.findall(r"([A-Za-z_][\w ]*?)\s*(\*+)?\s*([A-Za-z_]\w*)\s*(?:,|\))", line.split("(", 1)[1]):
                base = p[0].split()[-1] if p[0].split() else ""
                kinds[p[2]] = "ptr" if p[1] else ("float" if base in FLOAT_TYPES else "other")
        else:
            line = CAST_TO_FLOAT.sub(
                lambda c: f"(*({c.group(1)} *)&{c.group(2)})" if kinds.get(c.group(2)) == "ptr" else c.group(0), line)
            line = CAST_TO_POINTER.sub(
                lambda c: f"(*({c.group(1)} *)&{c.group(2)})" if kinds.get(c.group(2)) == "float" else c.group(0), line)
        out.append(line)
    return out
CLANG_FLAGS = ["-fsyntax-only", "--target=i686-pc-windows-msvc", "-std=gnu11", "-fms-extensions", "-w",
               "-ferror-limit=0", "-Dsize_t=ghidra_size_t",
               "-Wno-error=implicit-function-declaration", "-Wno-error=int-conversion",
               "-Wno-error=incompatible-pointer-types", "-Wno-error=incompatible-function-pointer-types",
               "-Wno-error=return-type", "-Wno-error=implicit-int"]
ERROR_LINE = re.compile(r"^(.*?):(\d+):\d+: error: (.*)$")

SIZE_CHECK = """#include "dra2_formats.h"
_Static_assert(sizeof(Bone) == 160, "Bone");
_Static_assert(sizeof(SkinnedVertex) == 60, "SkinnedVertex");
_Static_assert(sizeof(StaticVertex) == 44, "StaticVertex");
_Static_assert(sizeof(ShaderLayer) == 120, "ShaderLayer");
_Static_assert(sizeof(ResTocEntry) == 20, "ResTocEntry");
_Static_assert(sizeof(MeshDescriptor) == 44, "MeshDescriptor");
_Static_assert(sizeof(XboxWaveFormat) == 20, "XboxWaveFormat");
_Static_assert(sizeof(ResIndexEntry) == 12, "ResIndexEntry");
"""


def definitions(path):
    """(name, prototype) for each function definition in a preprocessed file."""
    lines = open(path, encoding="latin-1").read().split("\n")
    out = []
    i = 0
    while i < len(lines):
        if FUNC_HEADER.match(lines[i]):
            j = i + 1
            sig = []
            in_comment = False
            while j < len(lines) and lines[j].strip() != "{":
                s = lines[j].strip()
                if in_comment or s.startswith("/*"):
                    in_comment = not s.endswith("*/")
                elif s and not s.startswith("//"):
                    sig.append(s)
                j += 1
            text = " ".join(sig)
            names = PROTO_NAME.findall(text)
            if names and text:
                out.append((names[0], text + ";"))
            i = j
        i += 1
    return out


CAST_ERROR = re.compile(r"\.c:(\d+):(\d+): error: (pointer cannot be cast to type|operand of type '[^']*' cannot be "
                        r"cast to a pointer type)")
LVALUE_CHAIN = re.compile(r"[A-Za-z_]\w*(?:\s*\[[^\[\]]*\]|->\w+|\.\w+)*")


def matching_paren(s, i):
    """Index of the bracket closing the one at s[i] ('(' or '[')."""
    opener = s[i]
    closer = ")" if opener == "(" else "]"
    depth = 0
    for k in range(i, len(s)):
        if s[k] == opener:
            depth += 1
        elif s[k] == closer:
            depth -= 1
            if depth == 0:
                return k
    return -1


TYPE_IN_PARENS = re.compile(r"\(\s*(?:float|double|float10|[A-Za-z_][\w ]*?\s*\*+|int|uint|char|byte|short|ushort|"
                            r"undefined\d?)\s*\)")


def operand_end(line, j):
    """End index (inclusive) of the cast operand starting at j, and whether it is a plain lvalue. A cast binds to a
    unary expression: prefixes (-x, *p, &x), a nested cast, a parenthesised expression or a name, then postfix
    member accesses, subscripts and calls."""
    k = j
    while k < len(line) and line[k] in "-*&!~ ":
        k += 1
    prefixed = k > j
    if k < len(line) and line[k] == "(":
        close = matching_paren(line, k)
        if close < 0:
            return -1, False
        if TYPE_IN_PARENS.fullmatch(line[k:close + 1]):
            k = close + 1
            while k < len(line) and line[k] == " ":
                k += 1
            inner, _ = operand_end(line, k)
            return inner, False
        end, lvalue = close, False
    else:
        m = re.compile(r"[A-Za-z_]\w*").match(line, k)
        if not m:
            return -1, False
        end, lvalue = m.end() - 1, not prefixed
    while True:  # postfix operators
        n = end + 1
        m = re.compile(r"\s*(?:->|\.)\s*[A-Za-z_]\w*").match(line, n)
        if m:
            end = m.end() - 1
            continue
        if n < len(line) and line[n] in "[(":
            close = matching_paren(line, n)
            if close < 0:
                break
            if line[n] == "(":
                lvalue = False
            end = close
            continue
        break
    return end, lvalue and not prefixed


def rewrite_cast_at(line, col):
    """Rewrite the float<->pointer cast starting at column col (1-based) as a bit reinterpretation."""
    i = col - 1
    # clang points either at the cast or at its operand; a "(type)" ending right before col is the cast
    before = re.search(r"\(\s*[A-Za-z_][\w ]*\**\s*\)\s*$", line[:col - 1])
    if before and re.fullmatch(r"\(\s*(?:float|double|float10|[A-Za-z_][\w ]*\*+)\s*\)\s*", before.group(0)):
        i = before.start()
    elif i >= len(line) or line[i] != "(":
        i = line.rfind("(", 0, col)
        if i < 0:
            return line
    close = matching_paren(line, i)
    if close < 0:
        return line
    target = line[i + 1:close].strip()
    j = close + 1
    while j < len(line) and line[j] == " ":
        j += 1
    end, lvalue = operand_end(line, j)
    if end < 0:
        return line
    operand = line[j:end + 1]
    source = "float" if "*" in target else "void *"  # float -> pointer, or pointer -> float
    replacement = (f"(*({target} *)&({operand}))" if lvalue
                   else f"(*({target} *)&({source}){{{operand}}})")
    return line[:i] + replacement + line[end + 1:]


# Calls whose callee type doesn't fit the call: a vtable slot typed void * or with the wrong arity, a function whose
# recovered prototype lost or gained parameters, a global label used as a function. The call goes through 'code *'
# (int (), any arguments) instead, which is what the machine code does anyway.
CALL_ERROR = re.compile(r"\.c:(\d+):(\d+): error: (called object type '[^']*' is not a function or function pointer|"
                        r"too (?:many|few) arguments to function call|operand of type 'void' where arithmetic or "
                        r"pointer type is required|assigning to '[^']*'(?: \(aka '[^']*'\))? from incompatible type "
                        r"'void'|initializing '[^']*'(?: \(aka '[^']*'\))? with an expression of incompatible type "
                        r"'void')")
INDIRECT_CALL = re.compile(r"\(\*\s*([A-Za-z_]\w*(?:\s*(?:->|\.)\s*[A-Za-z_]\w*|\[[^\[\]]*\])*)\s*\)\s*\(")
# A global the code dereferences elsewhere (so declared as a pointer) but uses here as a number.
NUMBER_ERROR = re.compile(r"\.c:(\d+):\d+: error: (?:invalid operands to binary expression \('([^']*)'(?: \(aka '[^']*'\))? "
                          r"and '([^']*)'|passing '[^']*'(?: \(aka '[^']*'\))? to parameter of incompatible type "
                          r"'([^']*)'|"
                          r"assigning to 'undefined[1-8] \*' \(aka '[^']*'\) from incompatible type '([^']*)'|"
                          r"assigning to '([^']*)' from incompatible type 'undefined[1-8] \*')")
# A local Ghidra typed as a function pointer but indexed like a byte pointer (p[0x84]): index it as bytes.
FN_SUBSCRIPT_ERROR = re.compile(r"\.c:(\d+):(\d+): error: subscript of pointer to function type")
# A label typed as a plain number but used as a pointer: "G = (T *)G->next" (free lists) gets G read as T *.
MEMBER_ERROR = re.compile(r"\.c:(\d+):\d+: error: member reference base type 'undefined4'")
GLOBAL_AS_NUMBER = re.compile(r"(?<![\w&*>.])((?:_?DAT|PTR)_[0-9a-f]+)\b(?!\s*(?:\[|->|\())")


def rewrite_call_at(line, col):
    """Route the call at/around column col (1-based) through (code *)."""
    new = INDIRECT_CALL.sub(lambda m: f"((code *){m.group(1)})(", line)
    if new != line:
        return new
    # a direct call: the innermost '(' left of col that is still open at col, preceded by a name
    # ("too few arguments" points at the call's closing parenthesis, which belongs to the call itself)
    depth = 0
    start = min(col - 1, len(line) - 1)
    if start >= 0 and line[start] == ")":
        start -= 1
    for k in range(start, -1, -1):
        c = line[k]
        if c == ")":
            depth += 1
        elif c == "(":
            if depth == 0:
                m = re.search(r"([A-Za-z_]\w*)\s*$", line[:k])
                if m and m.group(1) not in ("if", "while", "switch", "return", "for", "sizeof"):
                    return line[:m.start(1)] + f"((code *){m.group(1)})" + line[m.end(1):]
            else:
                depth -= 1
    # col at the callee name itself
    m = re.match(r"[A-Za-z_]\w*", line[col - 1:]) if col - 1 < len(line) else None
    if m and line[col - 1 + m.end():].lstrip().startswith("("):
        return line[:col - 1] + f"((code *){m.group(0)})" + line[col - 1 + m.end():]
    return line


def rewrite_globals_as_numbers(line, kind, pointer_globals):
    return GLOBAL_AS_NUMBER.sub(
        lambda m: f"(*({kind} *)&{m.group(1)})" if m.group(1) in pointer_globals else m.group(0), line)


def fix_cast_errors(path, clang, includes, pointer_globals=frozenset()):
    """Compile, rewrite what each error points at (float<->pointer casts, mismatched calls, pointer globals used as
    numbers), repeat (nested cases)."""
    fixed = 0
    for _ in range(6):
        proc = subprocess.run([clang, *CLANG_FLAGS, *[f"-I{i}" for i in includes], path],
                              capture_output=True, text=True, encoding="latin-1")
        casts = {(int(m.group(1)), int(m.group(2))) for m in CAST_ERROR.finditer(proc.stderr)}
        calls = {(int(m.group(1)), int(m.group(2))) for m in CALL_ERROR.finditer(proc.stderr)}
        numbers = {}
        for m in NUMBER_ERROR.finditer(proc.stderr):
            types = [t for t in m.groups()[1:] if t]
            numbers[int(m.group(1))] = "float" if any(t in FLOAT_TYPES for t in types) else "uint"
        subscripts = {(int(m.group(1)), int(m.group(2))) for m in FN_SUBSCRIPT_ERROR.finditer(proc.stderr)}
        members = {int(m.group(1)) for m in MEMBER_ERROR.finditer(proc.stderr)}
        if not (casts or calls or numbers or subscripts or members):
            break
        lines = open(path, encoding="latin-1").read().split("\n")
        before = list(lines)
        for ln, col in sorted(subscripts, reverse=True):
            line = lines[ln - 1]
            m = re.match(r"[A-Za-z_]\w*", line[col - 1:])
            if m:
                lines[ln - 1] = line[:col - 1] + f"((byte *){m.group(0)})" + line[col - 1 + m.end():]
        for ln in members:
            line = re.sub(r"\(([A-Za-z_]\w*\s*\*+)\)\s*((?:_?DAT|PTR)_\w+)->",
                          lambda m: f"({m.group(1)})(({m.group(1)}){m.group(2)})->", lines[ln - 1])
            # an offset-named field of an untyped label is a byte offset from it
            lines[ln - 1] = re.sub(r"\b((?:_?DAT|PTR)_\w+)->field\d*_0x([0-9a-f]+)\b",
                                   r"((undefined1 *)\1)[0x\2]", line)
        for ln, col in sorted(casts | calls, reverse=True):  # right-to-left keeps earlier columns valid
            if ln > len(lines):
                continue
            if (ln, col) in casts:
                lines[ln - 1] = rewrite_cast_at(lines[ln - 1], col)
                continue
            new = rewrite_call_at(lines[ln - 1], col)
            # an argument on a continuation line: the call opens on one of the lines above
            back = 1
            while new == lines[ln - 1] and back <= 3 and ln - 1 - back >= 0:
                prev = lines[ln - 1 - back]
                fixed_prev = rewrite_call_at(prev, len(prev) + 1)
                if fixed_prev != prev:
                    lines[ln - 1 - back] = fixed_prev
                    break
                back += 1
            else:
                lines[ln - 1] = new
        for ln, kind in numbers.items():
            if ln <= len(lines):
                lines[ln - 1] = rewrite_globals_as_numbers(lines[ln - 1], kind, pointer_globals)
        changed = sum(1 for a, b in zip(before, lines) if a != b)
        if not changed:
            break
        fixed += changed
        with open(path, "w", encoding="latin-1") as f:
            f.write("\n".join(lines))
    return fixed


def dedupe_definitions(pp_files):
    """Ghidra allows two functions with the same name in one class (e.g. two 'ctor's); C doesn't. The second and later
    definitions of a name get the address as a suffix. Calls keep referring to the first."""
    seen = set()
    renamed = 0
    for p in pp_files:
        lines = open(p, encoding="latin-1").read().split("\n")
        changed = False
        i = 0
        while i < len(lines):
            m = FUNC_HEADER.match(lines[i])
            if m:
                addr = m.group(1)
                j = i + 1
                while j < len(lines) and lines[j].strip() != "{":
                    s = lines[j].strip()
                    names = PROTO_NAME.findall(s) if s and not s.startswith(("/*", "//", "*")) else []
                    if names:
                        name = names[0]
                        if name in seen:
                            lines[j] = re.sub(rf"\b{re.escape(name)}\s*\(", f"{name}_{addr}(", lines[j], count=1)
                            changed = True
                            renamed += 1
                        seen.add(name)
                        break
                    j += 1
                i = j
            i += 1
        if changed:
            with open(p, "w", encoding="latin-1") as f:
                f.write("\n".join(lines))
    return renamed


IMAGE_CONSTANT = re.compile(r"(?<![\w&])0x(00)?(29[2-9a-f][0-9a-f]{3}|2[a-f][0-9a-f]{4}|3[0-2][0-9a-f]{4})\b")
DATA_RANGES = ((0x292980, 0x2BF854), (0x2BF860, 0x326004))  # .rdata, .data (with its zero-filled tail)


def image_constant(m):
    value = int(m.group(2), 16)
    if any(lo <= value < hi for lo, hi in DATA_RANGES):
        return f"((int)&DAT_{value:08x})"
    return m.group(0)


STACK_REF = re.compile(r"\bstack0x([0-9a-f]{8})\b")
# a stack local's declaration: "  undefined1 local_158 [44];", "  float fStack_34;", "  int *piStack_10;"
FRAME_DECL = re.compile(r"^\s+([A-Za-z_][\w ]*?)\s*(\*+)?\s*((?:local|[a-zA-Z]{1,6}Stack)_([0-9a-f]+))"
                        r"\s*(\[\s*(\w+)\s*\])?\s*;\s*$")
SPLIT_REF = re.compile(r"\b_((param_\d+)|(local_[0-9a-f]+)|([a-zA-Z]{1,4}Stack_[0-9a-f]+))\b")
REGISTER_ESP = "register0x00000010"


def split_params(sig):
    """[(type, name)] of a function signature's parameter list ('void' -> [])."""
    open_ = sig.find("(")
    close = sig.rfind(")")
    body = sig[open_ + 1:close] if open_ >= 0 and close > open_ else ""
    if body.strip() in ("", "void"):
        return []
    parts, depth, cur = [], 0, ""
    for ch in body:
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
            continue
        depth += ch == "("
        depth -= ch == ")"
        cur += ch
    parts.append(cur)
    out = []
    for p in parts:
        m = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*$", p.strip())
        if m:
            out.append((p.strip()[:m.start(1)].strip(), m.group(1)))
    return out


INT_CAST = re.compile(r"\((int|uint|undefined4|undefined2|undefined1|short|ushort|char|byte|bool|"
                      r"[A-Za-z_]\w*\s*\*+)\s*\)\s*")
BITWISE = re.compile(r"\s[|&^]\s|\s(?:<<|>>)\s")


def float_bits(line, scalars, pointers):
    """Integer and pointer casts of float storage are bit copies in this game (it converts floats to integers only
    through __ftol / ROUND), but C converts the value: a pointer read through a float field became 0. So
    (int)f -> (int)*(uint *)&f, likewise f[i] / *(float *)p, and f = (float)(bits | 0x1000) -> *(uint *)&f = ..."""
    def lvalue_end(s, pos):
        """end of the float lvalue starting at s[pos], or -1"""
        m = re.match(r"\w+", s[pos:])
        if not m:
            return -1
        name, end = m.group(0), pos + m.end()
        if name in pointers and end < len(s) and s[end] == "[":
            depth = 0
            for k in range(end, len(s)):
                depth += s[k] == "["
                depth -= s[k] == "]"
                if depth == 0:
                    return k + 1
            return -1
        if name in scalars and not re.match(r"\s*[\[(]", s[end:]):
            return end
        return -1

    out, pos = [], 0
    for m in INT_CAST.finditer(line):
        if m.start() < pos or line[m.start() - 1:m.start()].isalnum():
            continue
        end = lvalue_end(line, m.end())
        if end < 0:
            continue
        out.append(line[pos:m.end()] + f"*(uint *)&{line[m.end():end]}")
        pos = end
    line = "".join(out) + line[pos:]

    am = re.match(r"^(\s*)(.+?) = \(float\)(\(.*\));(\s*)$", line)
    if am and lvalue_end(am.group(2), 0) == len(am.group(2)) and BITWISE.search(am.group(3)):
        line = f"{am.group(1)}*(uint *)&{am.group(2)} = {am.group(3)};{am.group(4)}"
    return line


ODD_STORE = re.compile(r"^(\s*)(\(?\s*\*\s*\(\s*undefined([3567])\s*\*\s*\)\s*)")


def odd_store(line):
    """'*(undefined3 *)p = v;' -> 'ghidra_store(p, v, 3);': undefined3/5/6/7 are wider C types, and a store through
    them wrote past the field (see ghidra_types.h)."""
    m = ODD_STORE.match(line)
    if not m or not line.rstrip().endswith(";"):
        return line
    rest = line[m.end():].rstrip()[:-1]
    wrapped = m.group(2).lstrip().startswith("(")  # "(*(undefined3 *)ADDR) = v;"
    # the assignment's "=" outside any parentheses (not ==, <=, >=, !=)
    depth = 1 if wrapped else 0
    for k, ch in enumerate(rest):
        depth += ch in "(["
        depth -= ch in ")]"
        if ch == "=" and depth == 0 and rest[k - 1:k] not in ("=", "<", ">", "!") and rest[k + 1:k + 2] != "=":
            lhs, value = rest[:k].strip(), rest[k + 1:].strip()
            if wrapped:  # drop the wrapper's ")"
                if not lhs.endswith(")"):
                    return line
                lhs = lhs[:-1].strip()
            if not lhs:
                return line
            return f"{m.group(1)}ghidra_store({lhs}, (unsigned long long)({value}), {m.group(3)});\n"
    return line


VCALL = "(**(code **)"


def matching_paren(s, i):
    """index of the ")" closing the "(" at s[i], or -1"""
    depth = 0
    for k in range(i, len(s)):
        if s[k] in "([":
            depth += 1
        elif s[k] in ")]":
            depth -= 1
            if depth == 0:
                return k
    return -1


def split_top(s, sep):
    """s split at the separators outside parentheses and brackets"""
    parts, depth, cur, k = [], 0, "", 0
    while k < len(s):
        ch = s[k]
        depth += ch in "(["
        depth -= ch in ")]"
        if depth == 0 and s.startswith(sep, k):
            parts.append(cur)
            cur, k = "", k + len(sep)
            continue
        cur += ch
        k += 1
    return parts + [cur]


def vcall_this(line, counter=None):
    """Virtual calls on objects of unknown type, (**(code **)(*obj + 8))(args), lose their "this": MSVC passes it in
    ECX and Ghidra doesn't show it. They become __thiscall calls with the object as "this", read once:
    ({ void *vcall_N = (void *)(obj); ((int (__thiscall *)(void *, ...))*(code **)(*(int *)vcall_N + 8))(vcall_N,
    args); })"""
    counter = counter if counter is not None else [0]
    out, pos = [], 0
    while True:
        i = line.find(VCALL, pos)
        if i < 0:
            break
        j = i + len(VCALL)
        inner, after = None, -1
        if line[j:j + 1] == "(":
            k = matching_paren(line, j)
            if k > 0:
                inner, after = line[j + 1:k], k + 1
        else:
            m = re.match(r"\*\s*\w+(?:\[[^\]]*\])?", line[j:])
            if m:
                inner, after = m.group(0), j + m.end()
        if inner is None or not inner.strip().startswith("*") or line[after:after + 2] != ")(":
            out.append(line[pos:j])
            pos = j
            continue
        end = matching_paren(line, after + 1)
        if end < 0:
            break
        args = line[after + 2:end].strip()
        parts = split_top(inner.strip()[1:], " + ")
        obj, slot = (" + ".join(parts[:-1]), parts[-1]) if len(parts) > 1 else (parts[0], "0")
        obj = vcall_this(obj, counter)
        arg_list = [vcall_this(a.strip(), counter) for a in split_top(args, ",")] if args else []

        def bare(e):
            e = re.sub(r"\(\s*[A-Za-z_][\w ]*\**\s*\)", "", e).strip()
            while e.startswith("(") and e.endswith(")") and matching_paren(e, 0) == len(e) - 1:
                e = e[1:-1].strip()
            return e
        if arg_list and bare(arg_list[0]) == bare(obj):
            arg_list = arg_list[1:]  # "this" already shown (a call-site signature override): not twice
        counter[0] += 1
        t = f"vcall_{counter[0]}"
        types = ", ".join(["void *"] + ["int" if "({" in a else f"__typeof__({a})" for a in arg_list])
        call_args = ", ".join([t] + arg_list)
        out.append(line[pos:i] + f"({{ void *{t} = (void *)({obj}); "
                   f"((int (__thiscall *)({types}))*(code **)(*(int *){t} + ({slot})))({call_args}); }})")
        pos = end + 1
    return "".join(out) + line[pos:]


SLOT_CALL = re.compile(r"\((?:\(code \*\)\s*|\*\s*)")


def slot_calls(line, counter=None):
    """Calls through a typed vtable slot, ((code *)obj->vftable->slot03_1de00f)() or
    (*obj->vftable->slot13_094170)(obj): the methods are __thiscall, but the first form passes no "this" and the
    second passes it on the stack (the slot's C type lists it as a parameter). Both become __thiscall calls with the
    object in ECX, read once; a leading copy of the object among the arguments is dropped."""
    counter = counter if counter is not None else [0]
    out, pos = [], 0
    for m in SLOT_CALL.finditer(line):
        if m.start() < pos:
            continue
        open_ = m.start()
        close = matching_paren(line, open_)
        if close < 0 or line[close + 1:close + 2] != "(":
            continue
        target = line[m.end():close].strip()
        tm = re.fullmatch(r"(.+)->vftable->(\w+)", target)  # slot03_1de00f, or a named one (Setup)
        if not tm:
            continue
        obj = tm.group(1).strip()
        if obj.count("(") != obj.count(")"):
            continue
        end = matching_paren(line, close + 1)
        if end < 0:
            continue
        args = line[close + 2:end].strip()
        arg_list = [a.strip() for a in split_top(args, ",")] if args else []

        def bare(e):
            e = re.sub(r"\(\s*[A-Za-z_][\w ]*\**\s*\)", "", e).strip()
            while e.startswith("(") and e.endswith(")") and matching_paren(e, 0) == len(e) - 1:
                e = e[1:-1].strip()
            return e
        if arg_list and bare(arg_list[0]) == bare(obj):
            arg_list = arg_list[1:]
        counter[0] += 1
        t = f"slot_{counter[0]}"
        types = ", ".join(["void *"] + ["int" if "({" in a else f"__typeof__({a})" for a in arg_list])
        call_args = ", ".join([t] + arg_list)
        out.append(line[pos:open_] + f"({{ void *{t} = (void *)({obj}); "
                   f"((int (__thiscall *)({types}))(((__typeof__({obj}))({t}))->vftable->{tm.group(2)}))"
                   f"({call_args}); }})")
        pos = end + 1
    return "".join(out) + line[pos:]


SLOT0 = re.compile(r"\(\*\(code \*\)\*\*\(\w+ \*\*\)")


def slot0_calls(line, counter=None):
    """Calls through the first vtable slot, (*(code *)**(undefined4 **)obj)(args): the same __thiscall rewrite as
    vcall_this (an array of objects destroyed through slot 0, the deleting destructor, ran with no "this")."""
    counter = counter if counter is not None else [0]
    out, pos = [], 0
    for m in SLOT0.finditer(line):
        if m.start() < pos:
            continue
        j = m.end()
        if line[j:j + 1] == "(":
            k = matching_paren(line, j)
            if k < 0:
                continue
            obj, after = line[j + 1:k], k + 1
        else:
            om = re.match(r"\w+", line[j:])
            if not om:
                continue
            obj, after = om.group(0), j + om.end()
        if line[after:after + 2] != ")(":
            continue
        end = matching_paren(line, after + 1)
        if end < 0:
            continue
        args = line[after + 2:end].strip()
        arg_list = [a.strip() for a in split_top(args, ",")] if args else []

        def bare(e):
            e = re.sub(r"\(\s*[A-Za-z_][\w ]*\**\s*\)", "", e).strip()
            while e.startswith("(") and e.endswith(")") and matching_paren(e, 0) == len(e) - 1:
                e = e[1:-1].strip()
            return e
        if arg_list and bare(arg_list[0]) == bare(obj):
            arg_list = arg_list[1:]
        counter[0] += 1
        t = f"slot0_{counter[0]}"
        types = ", ".join(["void *"] + ["int" if "({" in a else f"__typeof__({a})" for a in arg_list])
        call_args = ", ".join([t] + arg_list)
        out.append(line[pos:m.start()] + f"({{ void *{t} = (void *)({obj}); "
                   f"((int (__thiscall *)({types}))**(code ***){t})({call_args}); }})")
        pos = end + 1
    return "".join(out) + line[pos:]


SOURCE_PATCHES = {}  # address -> [(old, new)], from analysis/source_patches.tsv
SOURCE_PATCHES_APPLIED = set()


def load_source_patches(decomp):
    path = os.path.join(decomp, "..", "analysis", "source_patches.tsv")
    if not os.path.exists(path):
        return
    for row in csv.DictReader(open(path), delimiter="\t"):
        SOURCE_PATCHES.setdefault(row["address"], []).append((row["old"], row["new"]))


def fix_pseudo_storage(lines, class_methods):
    """Give Ghidra's pseudo-variables real storage, function by function:
    - stack0xNNNNNNNN (a stack slot relative to the stack pointer at entry): positive offsets are the incoming
      arguments, addressed from the first stack parameter; negative ones are local buffers Ghidra never made into
      variables, which become a per-function byte array ghidra_frame
    - _param_N / _local_X / _xStack_X (a wider view of the same slot): an access through that variable's address
    - register0x00000010 (ESP at entry): the frame address + 4, so +4 is the first stack argument
    - in_stack_ffffXXXX: the same frame slot as stack0xffffXXXX (an outgoing argument area the code filled)
    - a method of the function's own class called without its class prefix (vf01(this, ...)): qualified
    Everything stays C; the semantics match the original stack layout as far as clang's frame allows."""
    out = []
    i = 0
    while i < len(lines):
        m = FUNC_HEADER.match(lines[i].rstrip("\n"))
        if not m:
            out.append(lines[i])
            i += 1
            continue
        # header, signature up to the opening brace, body up to the closing brace (both at column 0)
        j = i + 1
        while j < len(lines) and lines[j].rstrip("\n") != "{":
            j += 1
        k = j + 1
        while k < len(lines) and lines[k].rstrip("\n") != "}":
            k += 1
        if j >= len(lines) or k >= len(lines):
            out.extend(lines[i:])
            break
        sig_lines, body = lines[i:j + 1], lines[j + 1:k]
        # analysis/source_patches.tsv: one-off fixes for decompiler mistakes no general rule covers, on the raw text
        for old, new in SOURCE_PATCHES.get(m.group(1), []):
            joined = "".join(body)
            if old in joined:
                body = joined.replace(old, new).splitlines(keepends=True)
                SOURCE_PATCHES_APPLIED.add((m.group(1), old))
        sig = " ".join(l.strip() for l in sig_lines[1:-1] if not l.strip().startswith(("/*", "*", "//")))
        params = split_params(sig)
        names = [n for _, n in params]
        conv_regs = 1 if "__thiscall" in sig else 2 if "__fastcall" in sig else 0
        stack_params = names[conv_regs:]
        text = "".join(body)
        # a variable-length local: ESP is restored from EBP on return (callees' argument pops can differ)
        decls = ["  char ghidra_keep_esp[ghidra_dynamic_one]; (void)ghidra_keep_esp;\n"]

        # Stack locals keep their original layout. Ghidra names them by their offset below the entry stack pointer
        # (local_158, auStack_104, fStack_34) and often splits one structure into several of them; laid out
        # separately by clang, a callee filling the structure through the first piece would overrun it. So they all
        # live in one frame array at their original offsets, like the unnamed stack0xffffff.. slots.
        frame_vars = {}
        in_stack = {}  # in_stack_fffffed8: the same slot as stack0xfffffed8 (an argument area the code filled)
        arg_slots = {}  # pvStack0000001c: the caller's argument slot at entry ESP + 0x1c
        kept_body = []
        in_decls = True  # Ghidra declares every local in one block at the top, ended by a blank line
        for line in body:
            if in_decls and not line.strip():
                in_decls = False
            sm = re.match(r"^\s+([A-Za-z_][\w ]*?)\s*(\*+)?\s*(in_stack_([0-9a-f]{8}))\s*;\s*$", line) \
                if in_decls else None
            if sm and int(sm.group(4), 16) >= 0x80000000:
                in_stack[sm.group(3)] = (sm.group(1).strip() + (" " + sm.group(2) if sm.group(2) else ""),
                                         0x100000000 - int(sm.group(4), 16))
                continue
            # pvStack0000001c / iStack00000008: a slot in the caller's argument area (above the return address)
            # that the function reuses as a variable. It must be that slot: the code also takes its address as
            # "&param_1 + 24".
            am = re.match(r"^\s+([A-Za-z_][\w ]*?)\s*(\*+)?\s*([a-z]{1,6}Stack([0-9a-f]{8}))\s*;\s*$", line) \
                if in_decls else None
            if am and 4 <= int(am.group(4), 16) < 0x80000000:
                arg_slots[am.group(3)] = (am.group(1).strip() + (" " + am.group(2) if am.group(2) else ""),
                                          int(am.group(4), 16))
                continue
            dm = FRAME_DECL.match(line) if in_decls else None
            if dm and dm.group(1).split()[0] not in ("return", "goto", "case", "else", "do"):
                frame_vars[dm.group(3)] = (dm.group(1).strip() + (" " + dm.group(2) if dm.group(2) else ""),
                                           int(dm.group(4), 16), dm.group(6))
                continue
            kept_body.append(line)
        body = kept_body
        text = "".join(body)

        # stack0x...
        offsets = sorted({int(s, 16) for s in STACK_REF.findall(text)})
        negative = [0x100000000 - o for o in offsets if o >= 0x80000000] + [off for _, off in in_stack.values()]
        deepest = max(negative + [off for _, off, _ in frame_vars.values()] + [0])
        frame = deepest + 16 if deepest else 0
        if frame:
            # 4-byte aligned like the Xbox frame: a wider alignment makes clang realign ESP and address locals from
            # it, and then a callee that pops a different argument size than its decompiled prototype says shifts
            # every local (EBP-based frames survive that, the epilogue restores ESP)
            decls.append(f"  undefined1 ghidra_frame[{frame:#x}] __attribute__((aligned(4)));"
                         f" /* the stack frame below the entry stack pointer, original layout */\n")

        def frame_var(name):
            t, off, count = frame_vars[name]
            if count:
                return f"(*({t} (*)[{count}])&ghidra_frame[{frame - off:#x}])"
            return f"(*({t} *)&ghidra_frame[{frame - off:#x}])"
        first = stack_params[0] if stack_params else None
        if (offsets and any(o < 0x80000000 for o in offsets) or arg_slots) and not first:
            decls.append("  undefined1 ghidra_args[0x1000]; /* no stack parameters: stand-in argument area */\n")
        arg_exprs = {name: (f"(*({t} *)&((undefined1 *)&{first})[{o - 4:#x}])" if first
                            else f"(*({t} *)&ghidra_args[{o:#x}])") for name, (t, o) in arg_slots.items()}
        arg_ref = re.compile(r"(?<![\w.>])(" + "|".join(map(re.escape, arg_exprs)) + r")\b") if arg_exprs else None

        def stack_slot(mm):
            o = int(mm.group(1), 16)
            if o >= 0x80000000:
                return f"ghidra_frame[{frame - (0x100000000 - o):#x}]"
            if first:
                return f"((undefined1 *)&{first})[{o - 4}]"
            return f"ghidra_args[{o:#x}]"

        # _param_N, _local_X, _xStack_X
        split = {}
        for mm in SPLIT_REF.finditer(text):
            name = mm.group(1)
            if name in split:
                continue
            ptr = re.search(rf"\*\s*\(?\s*_{name}\b|\b_{name}\s*(?:\[|->)", text) is not None
            t = "undefined4 *" if ptr else "undefined4"
            base_declared = re.search(rf"\b{re.escape(name)}\s*(?:\[[^\]]*\])?\s*;", text) or name in names
            if name in frame_vars:
                split[name] = f"(*({t} *)&{frame_var(name)})"
            elif base_declared:
                split[name] = f"(*({t} *)&{name})"
            elif name.startswith("param_") and first:
                n = int(name.split("_")[1])
                split[name] = f"(*({t} *)((undefined1 *)&{first} + {4 * (n - 1)}))"
            else:
                decls.append(f"  {t} _{name}; /* split variable with no base */\n")

        def split_ref(mm):
            return split.get(mm.group(1), mm.group(0))

        if REGISTER_ESP in text:
            # the stack pointer at entry: above the saved EBP, so [+4] is the first stack argument as on the Xbox
            decls.append(f"  undefined1 *{REGISTER_ESP} = (undefined1 *)__builtin_frame_address(0) + 4;\n")

        # bare method names of the function's own class (or of a class named on the same line)
        # by now Class::method is spelled Class__method; the class is the part before the last "__"
        qual = m.group(2).replace("::", "__")
        cls = qual.rsplit("__", 1)[0] if "__" in qual.lstrip("_") else None
        if cls not in class_methods:
            cls = None

        def qualify(line):
            # Ghidra leaves a name bare only for the function's own class, so that class goes first
            classes = [cls] if cls else []
            classes += [c for c in re.findall(r"\b([A-Za-z_]\w*?)__\w+", line) if c in class_methods and c != cls]
            for c in classes:
                for meth in class_methods.get(c, ()):
                    if meth in names or meth == c:
                        continue
                    line = re.sub(rf"(?<![\w.>&:]){re.escape(meth)}\b(?=\s*[(,)])(?!\s*=)",
                                  f"{c}__{meth}", line)
                    line = re.sub(rf"&{re.escape(meth)}\b", f"&{c}__{meth}", line)
            return line

        # float storage holding integers or pointers (Ghidra typed a register or field float because it was also
        # moved through the FPU): see float_bits
        float_scalars, float_pointers = set(), set()
        for t, n in params:
            (float_pointers if re.fullmatch(r"float\s*\*", t.strip()) else
             float_scalars if t.strip() == "float" else set()).add(n)
        for line in body:
            if not line.strip():
                break
            dm = re.match(r"^\s+float\s*(\*?)\s*(\w+)\s*;", line)
            if dm:
                (float_pointers if dm.group(1) else float_scalars).add(dm.group(2))

        for name, (t, off) in in_stack.items():
            frame_vars[name] = (t, off, None)
        frame_ref = re.compile(r"(?<![\w.>])(" + "|".join(map(re.escape, frame_vars)) + r")\b") if frame_vars else None
        # "while (X = Y, cond) { ...; Y = X; }": the decompiler's copy between two names for one value. When the
        # loop also writes X through its address (&X: bytes copied into it one at a time), the copy at the top of
        # each iteration undid those writes; Y still takes X at the end of the iteration, so the copy goes.
        alias_copies = [(mm.group(1), mm.group(2)) for mm in re.finditer(r"while \((\w+) = (\w+), ", text)
                        if re.search(rf"&{re.escape(mm.group(1))}\b", text)]

        new_body = []
        for line in body:
            for x, y in alias_copies:
                line = line.replace(f"while ({x} = {y}, ", "while (")
            if float_scalars or float_pointers:
                line = float_bits(line, float_scalars, float_pointers)
            if frame_ref:
                line = frame_ref.sub(lambda mm: frame_var(mm.group(1)), line)
            if arg_ref:
                line = arg_ref.sub(lambda mm: arg_exprs[mm.group(1)], line)
            line = STACK_REF.sub(stack_slot, line)
            line = SPLIT_REF.sub(split_ref, line)
            if "undefined" in line:
                line = odd_store(line)
            if cls or "__" in line:
                line = qualify(line)
            new_body.append(line)
        out.extend(sig_lines)
        out.extend(decls)
        out.extend(new_body)
        out.append(lines[k])
        i = k + 1
    return out


def preprocess(src, dst, class_methods=None):
    out = []
    for line in open(src, encoding="latin-1"):
        if line.startswith("#include"):
            line = '#include "program_types.h"\n#include "protos.h"\n#include "globals.h"\n'
        # 10-byte x87 values (returns, locals, casts) are floating point; float10 is what the code does with them
        line = ARRAY10_RETURN.sub(r"\1float10 ", line)
        line = re.sub(r"^(\s+)undefined1\s+(\w+)\s*\[10\];", r"\1float10 \2;", line)
        line = re.sub(r"\(undefined1\s*\[10\]\)", "(float10)", line)
        line = SWITCH_LINE.sub(r"\1switch((int)(\2)) {", line)
        line = CASE_FLOAT.sub(r"case \1:", CASE_POINTER.sub(r"case \1:", line))
        line = re.sub(r"\bNAN\b(?!\s*\()", '__builtin_nanf("")', line)
        if VCALL in line:
            line = vcall_this(line)
        if "->vftable->" in line:
            line = slot_calls(line)
        if "(*(code *)**(" in line:
            line = slot0_calls(line)
        # "*(int *)(&DAT_0031fa00 + iVar7)": the decompiler adds a byte offset to an untyped label (for a typed one
        # it would index, (&G)[i]); declared with a wider type, C scaled the offset
        line = re.sub(r"(\*\s*\([^()]*\*\s*\)\s*\(\s*)&(_?(?:DAT|PTR)_[0-9a-f]{8})(\s*[+-])",
                      r"\1(undefined1 *)&\2\3", line)
        # MSVC C++ exception frames built by hand on the Xbox (fs:[0] chain + a pointer to the function's unwind
        # record in 0x1ee000-0x1fa000). A port doesn't use them: the frame links go, fs:[0] reads as 0, and the
        # unwind-record address becomes null. (unaff_FS_OFFSET is otherwise an uninitialised pointer.)
        line = re.sub(r"^(\s*)\*unaff_FS_OFFSET\s*=[^;]*;", r"\1/* SEH frame link removed */;", line)
        if not re.match(r"^\s*[A-Za-z_][\w ]*\*+\s*unaff_FS_OFFSET;", line):  # leave the declaration alone
            line = re.sub(r"\*unaff_FS_OFFSET\b", "0", line)
        line = re.sub(r"&(?:PTR_)?LAB_(?:001e[ef]|001f[0-9])[0-9a-f]{3}\b", "(void *)0", line)
        # "&DAT_00000014" is the decompiler showing a small integer constant (0x14) as the address of a label; outside
        # the image there is no such label, so it is the constant itself
        line = re.sub(r"&_*(?:DAT|UNK|PTR)_(0000[0-9a-f]{4})\b", r"((undefined1 *)0x\1)", line)
        # An integer constant inside .rdata/.data is an address the decompiler didn't turn into a label (array bounds,
        # objects passed by address, ...). Those sections are regenerated elsewhere in the port's image, so it becomes
        # the address of a label there (tools/gen_data.py defines DAT_<address>). Case labels must stay constants.
        if not re.match(r"\s*case\b", line):
            line = IMAGE_CONSTANT.sub(image_constant, line)
        # "(X_fn)0x0" is a null function pointer; plain 0 compares with anything it is used against
        line = re.sub(r"\(\w+_fn\s*\*?\)\s*0x0\b", "0", line)
        line = CASE_DENORMAL.sub(lambda m: f"case {round(float(m.group(1)) / 2.0 ** -149)}:", line)
        line = PARTIAL.sub(lambda m: f"(*({PARTIAL_TYPES.get(m.group(3), 'unsigned int')} *)"
                                     f"((char *)&{m.group(1)} + {m.group(2)}))", line)
        out.append(VTABLE_REF.sub(r"&\1_data", QUALIFIED.sub(r"\1__\2", line)))
    out = fix_float_pointer_casts(out)
    if class_methods is not None:
        out = fix_pseudo_storage(out, class_methods)
    text = "".join(out)
    vtables = set(re.findall(r"&(\w+_vftable)_data\b", text))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="latin-1") as f:
        f.write(text)
    return set(GLOBAL_ID.findall(text)), vtables


def function_ranges(path):
    ranges = []
    for n, line in enumerate(open(path, encoding="latin-1"), 1):
        m = FUNC_HEADER.match(line.rstrip("\n"))
        if m:
            ranges.append((n, m.group(1), m.group(2)))
    return ranges


def compile_file(args):
    clang, path, includes = args
    proc = subprocess.run([clang, *CLANG_FLAGS, *[f"-I{i}" for i in includes], path],
                          capture_output=True, text=True, encoding="latin-1")
    errors = []
    for line in proc.stderr.splitlines():
        m = ERROR_LINE.match(line)
        if m and os.path.normcase(os.path.abspath(m.group(1))) == os.path.normcase(os.path.abspath(path)):
            errors.append((int(m.group(2)), m.group(3)))
        elif m:
            errors.append((0, f"[{os.path.basename(m.group(1))}] {m.group(3)}"))
    return path, errors


def main():
    decomp, clang = sys.argv[1], sys.argv[2]
    jobs = next((int(a.split("=")[1]) for a in sys.argv[3:] if a.startswith("--jobs=")), os.cpu_count())
    build = os.path.join(decomp, "build")
    program_h = os.path.join(build, "program.h")
    if not os.path.exists(program_h):
        raise SystemExit("build/program.h missing: run ExportHeader.java first")
    program_text = open(program_h, encoding="latin-1").read()
    load_source_patches(decomp)

    # format struct sizes
    check = os.path.join(build, "format_sizes.c")
    with open(check, "w") as f:
        f.write(SIZE_CHECK)
    _, errs = compile_file((clang, check, [decomp]))
    print("format struct size check:", "ok" if not errs else errs)

    sources = []
    for dp, _, fns in os.walk(os.path.join(decomp, "src")):
        for fn in sorted(fns):
            if fn.endswith(".c"):
                sources.append(os.path.join(dp, fn))
    globals_used = set()
    vtables_used = set()
    pp_files = []
    shutil.rmtree(os.path.join(build, "pp"), ignore_errors=True)  # no files left over from an older export
    # every class's methods (for qualifying calls Ghidra printed without the class)
    class_methods = collections.defaultdict(set)
    for s in sources:
        for m in re.finditer(r"(?m)^// [0-9a-f]{8} ([A-Za-z_]\w*)::(~?\w+) \(", open(s, encoding="latin-1").read()):
            class_methods[m.group(1)].add(m.group(2))
    for s in sources:
        dst = os.path.join(build, "pp", os.path.relpath(s, os.path.join(decomp, "src")))
        g, v = preprocess(s, dst, class_methods)
        globals_used |= g
        vtables_used |= v
        pp_files.append(dst)

    print("duplicate definition names made unique:", dedupe_definitions(pp_files))

    # prototypes from the definitions themselves; drop the database prototypes for those functions
    protos = {}
    for p in pp_files:
        for name, proto in definitions(p):
            protos.setdefault(name, proto)
    kept = []
    seen = set()
    split = program_text.split("/* ---- function prototypes ---- */")
    for line in split[1].splitlines():
        m = PROTO_NAME.search(line)
        if m and line.rstrip().endswith(");"):
            names = PROTO_NAME.findall(line)
            # the function name is the identifier directly before the parameter list's '('
            fname = names[0] if names else None
            # same name twice (e.g. the CRT's narrow and wide _write_char copies): keep the first
            if fname in protos or fname in seen:
                continue
            seen.add(fname)
            # Ghidra array types C can't spell this way: x87 returns and pointer-to-array parameters
            line = re.sub(r"^(?:undefined1\[10\]|unkbyte10\b)", "float10", line)
            line = re.sub(r"\bundefined1\[\d+\] \*", "void *", line)
        kept.append(line)
    with open(os.path.join(build, "program_types.h"), "w", encoding="latin-1") as f:
        f.write(BARE_NAME_LINE.sub("", split[0]) + "/* ---- prototypes of functions not defined in src/ ---- */\n" +
                "\n".join(kept))
    with open(os.path.join(build, "protos.h"), "w", encoding="latin-1") as f:
        f.write("/* Generated by tools/compile_check.py: prototypes taken from the decompiled definitions. */\n")
        f.write("#pragma once\n" + "\n".join(protos.values()) + "\n")
    defined = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", program_text)) | set(protos)

    # globals the code uses as pointers (one pass over all files), and the pointer type the decompiler gives each
    pointer_globals = set()
    indexed_globals = set()
    arrow_globals = set()  # used as G->field: a structure pointer, whatever else it is cast to
    # Scalar labels are as wide as their accesses: a char literal ("G == '\0'", "G = '\x01'") is a byte access,
    # and no label is wider than the distance to the next one (analysis/data_symbols.tsv). Declared 4 bytes, the
    # flag byte at 0x2f7b14 also read its neighbour 0x2f7b15.
    byte_globals = set()
    float_globals = set()  # declared float; integer stores and casts of them become bit copies (below)
    int_globals = set()  # used with an integer-only operator: never a float
    plain_deref = set()  # "*G" with no cast: the code reads what G points at with G's own element type
    pointer_copies = []  # "H = G + ...;": G has H's pointer type
    label_width = {}
    symbols_tsv = os.path.join(decomp, "..", "analysis", "data_symbols.tsv")
    if os.path.exists(symbols_tsv):
        by_addr = {}
        for row in csv.DictReader(open(symbols_tsv), delimiter="\t"):
            if re.fullmatch(r"[0-9a-f]{8}", row["address"]) and "." not in row["name"]:
                by_addr.setdefault(int(row["address"], 16), row["name"])
        addrs = sorted(by_addr)
        for a, b in zip(addrs, addrs[1:]):
            width = b - a
            for name in (by_addr[a], "_" + by_addr[a]):
                label_width[name] = width
        # labels Ghidra names by address (DAT_002f7b14) that the table doesn't list are bounded the same way
        for a, b in zip(addrs, addrs[1:]):
            label_width.setdefault(f"DAT_{a:08x}", b - a)
    wide_globals = {}
    wide_returns = dict((n, t) for t, n in re.findall(
        rf"^({WIDE_TYPES})\s+(?:__\w+\s+)?(\w+)\s*\(", "\n".join(protos.values()) + "\n" + program_text, re.M))
    cast_votes = collections.defaultdict(collections.Counter)
    for p in pp_files:
        text = open(p, encoding="latin-1").read()
        for m in GLOBAL_POINTER_USE.finditer(text):
            pointer_globals.add(next(g for g in m.groups() if g))
        for m in re.finditer(rf"\b{_G}\s*\[", text):
            indexed_globals.add(m.group(1))
        for m in re.finditer(rf"\b{_G}\s*->", text):
            arrow_globals.add(m.group(1))
        for m in re.finditer(rf"(?<![\w)\]])\*\s*{_G}\b(?!\s*[\[(])", text):
            plain_deref.add(m.group(1))
        # "(float10)DAT_00311d48": a float label (for an integer one the decompiler shows (float10)(int)G)
        for m in re.finditer(rf"\((?:float10|float|double)\)\s*{_G}\b(?!\s*[\[(])", text):
            float_globals.add(m.group(1))
        # ... unless an integer-only operator uses it too (DAT_002f42fc % 900: a frame counter)
        for m in re.finditer(rf"(?<![\w.>*&]){_G}\s*(?:%|<<|>>|\+\+|--|[&|^](?![&|=]))|"
                             rf"(?:%|<<|>>|\+\+|--)\s*{_G}\b", text):
            int_globals.add(m.group(1) or m.group(2))
        for m in re.finditer(rf"^\s*{_G}\s*=\s*{_G}\s*[+;]", text, re.M):
            pointer_copies.append((m.group(1), m.group(2)))
        for m in re.finditer(rf"(?<![\w.>*&]){_G}\s*(?:[=!]=|=)\s*'", text):
            byte_globals.add(m.group(1))
        for m in re.finditer(rf"'\s*[=!]=\s*{_G}\b", text):
            byte_globals.add(m.group(1))
        # indexed: "(&G)[iVar7] == '\0'", "(&G)[i] = '\x01'": a byte array, so the index is a byte offset
        for m in re.finditer(rf"\(&{_G}\)\[[^\]]*\]\s*(?:[=!]=|=)\s*'", text):
            byte_globals.add(m.group(1))
        # 8-byte values stored into a label (a 64-bit counter, a double): the label is that wide
        for func in re.split(r"(?m)^// [0-9a-f]{8} ", text):
            # "piVar3 = G;" with "int *piVar3;": G holds an int * (a vote like a cast)
            pointer_locals = dict((n, re.sub(r"\s+", " ", t).strip() + " " + s) for t, s, n in
                                  re.findall(r"^\s+([A-Za-z_][\w ]*?)\s*(\*+)\s*(\w+)\s*;", func, re.M))
            for m in re.finditer(rf"^\s+(\w+)\s*=\s*{_G}\s*;", func, re.M):
                t = pointer_locals.get(m.group(1))
                if t:
                    cast_votes[m.group(2)][t] += 1
            wide_locals = dict((n, t) for t, n in re.findall(rf"^\s+({WIDE_TYPES})\s+(\w+)\s*;", func, re.M))
            for m in re.finditer(rf"(?<![\w.>*&]){_G}\s*=\s*(\w+)\s*([(;])", func):
                t = wide_returns.get(m.group(2)) if m.group(3) == "(" else wide_locals.get(m.group(2))
                if t:
                    wide_globals[m.group(1)] = t
            for m in re.finditer(rf"(?<![\w.>*&]){_G}\s*=\s*0x([0-9a-f]{{9,16}})\s*;", func):
                wide_globals.setdefault(m.group(1), "undefined8")
        for m in re.finditer(rf"(?<![\w.>*]){_G}\s*=\s*\(\s*([A-Za-z_]\w*\s*\*+)\s*\)", text):
            cast_votes[m.group(1)][re.sub(r"\s+", " ", m.group(2)).replace(" *", " *")] += 1
    known_types = set(re.findall(r"typedef\s[^;]*?\b(\w+)\s*;", program_text))
    known_types |= set(re.findall(r"\b(?:struct|union|enum)\s+(\w+)", program_text))
    known_types |= {"void", "char", "int", "uint", "byte", "short", "ushort", "float", "double", "undefined",
                    "undefined1", "undefined2", "undefined4", "undefined8", "code", "dword", "word", "bool"}
    primitive = {"void", "char", "int", "uint", "byte", "short", "ushort", "float", "double", "undefined",
                 "undefined1", "undefined2", "undefined4", "undefined8", "code", "dword", "word", "bool"}
    pointer_types = {}
    arrow_types = {}  # G->field on a label typed otherwise: the structure pointer type assigned to it
    for g, votes in cast_votes.items():
        t = votes.most_common(1)[0][0]
        base, stars = t.replace("*", "").strip(), t.count("*")
        if base in known_types:
            pointer_types[g] = f"{base} {'*' * stars}"
        if g in arrow_globals and (base in primitive or stars != 1):
            structs = [s for s, _ in votes.most_common() if s.count("*") == 1 and
                       s.replace("*", "").strip() not in primitive and s.replace("*", "").strip() in known_types]
            if structs:
                arrow_types[g] = structs[0].replace("*", "").strip()
    # labels with no cast of their own take the type of the typed label they are copied (or offset) into
    for _ in range(3):
        for h, g in pointer_copies:
            if g not in pointer_types and h in pointer_types and g in pointer_globals:
                pointer_types[g] = pointer_types[h]
    # the rest: dereferenced as "*G", a dword pointer (what the game's untyped pointers mostly are); otherwise a
    # byte pointer ("*(T *)(G + 8)" style)
    for g in pointer_globals:
        if g not in pointer_types and g in plain_deref:
            pointer_types[g] = "undefined4 *"

    # Ghidra types an untyped label per function: where a function treats a pointer label as an integer
    # ("iVar2 = G;" into an int local, "(int)G"), its "G + 4" is 4 bytes on, not 4 elements: there it becomes
    # "(int)G + 4". Labels declared as byte pointers scale the same either way.
    scaled = {g for g, t in pointer_types.items() if t.replace(" ", "") not in ("undefined1*", "char*", "byte*")}
    scaled |= {g for g in pointer_globals if g not in pointer_types and g in indexed_globals}
    arrow_ref = re.compile(r"(?<![\w.>])(" + "|".join(map(re.escape, arrow_types)) + r")\s*->") if arrow_types else None

    def integer_view(func):
        int_locals = set(re.findall(r"^\s+(?:int|uint|undefined4|dword)\s+(\w+)\s*;", func, re.M))
        views = {m.group(2) for m in re.finditer(rf"^\s+(\w+)\s*=\s*{_G}\s*;", func, re.M) if m.group(1) in int_locals}
        views |= set(re.findall(rf"\((?:int|uint)\)\s*{_G}\b", func))
        return views & scaled

    # "*(int *)(G + 4)": a cast dereference of an offset label is a byte offset (for an int * the decompiler would
    # print G[1]), whatever type the label got here. Declared int *, C read G + 0x10.
    cast_offset = re.compile(r"(\*\s*\([^()]*\*\s*\)\s*\(\s*)(" + "|".join(map(re.escape, sorted(scaled))) +
                             r")(\s*[+-]\s*)") if scaled else None
    # float labels: integer constants stored into them and integer casts of them are bit copies, as for float locals
    floats = sorted(g for g in float_globals
                    if g not in pointer_globals and g not in wide_globals and g not in int_globals)
    if floats:
        alt = "|".join(map(re.escape, floats))
        float_store = re.compile(rf"(?<![\w.>*&])({alt})\s*=\s*(-?(?:0x[0-9a-f]+|\d+))\s*;")
        float_cast = re.compile(rf"\((int|uint|undefined4)\)\s*({alt})\b")
    for p in pp_files:
        text = open(p, encoding="latin-1").read()
        new = arrow_ref.sub(lambda mm: f"(({arrow_types[mm.group(1)]} *){mm.group(1)})->", text) if arrow_ref else text
        if cast_offset:
            new = cast_offset.sub(r"\1(int)\2\3", new)
        if floats:
            new = float_store.sub(r"*(uint *)&\1 = \2;", new)
            new = float_cast.sub(r"(\1)*(uint *)&\2", new)
        parts = re.split(r"(?m)(^// [0-9a-f]{8} .*$)", new)
        for k in range(2, len(parts), 2):
            views = integer_view(parts[k])
            if views:
                ref = re.compile(r"(?<![\w.>)])(?<!\(int\))(?<!\(uint\))(" + "|".join(map(re.escape, views)) +
                                 r")(\s*[+-]\s*(?:0x|\d))")
                parts[k] = ref.sub(r"(int)\1\2", parts[k])
        new = "".join(parts)
        if new != text:
            with open(p, "w", encoding="latin-1") as f:
                f.write(new)

    with open(os.path.join(build, "globals.h"), "w") as f:
        f.write("/* Generated by tools/compile_check.py: global labels used by the decompiled code. */\n")
        f.write("#pragma once\n")
        for v in sorted(vtables_used):
            f.write(f"extern struct {v} {v}_data; /* the vtable itself, in .rdata */\n")
        for g in sorted(globals_used - defined):
            if g.startswith(("LAB_", "switchD_", "caseD_", "code_r0x")):
                f.write(f"extern void {g}(void);\n")
            elif g.startswith(("s_", "u_")):
                f.write(f"extern char {g}[];\n")
            elif g.startswith("stack0x"):
                f.write(f"extern undefined1 {g}; /* Ghidra pseudo-variable for a caller's stack slot */\n")
            elif re.fullmatch(r"[a-z]{1,4}Ram[0-9a-f]{8}", g):
                f.write(f"extern {ram_type(g)}{g}; /* memory at an absolute address */\n")
            elif g.startswith("luakey_"):
                f.write(f"extern char {g}[]; /* Lua key string label */\n")
            elif g.startswith(("_param_", "_local_", "register0x")) or re.fullmatch(r"_[a-zA-Z]{1,4}Stack_[0-9a-f]+", g):
                ptype = "undefined1 *" if g in pointer_globals else "undefined4 "
                f.write(f"extern {ptype}{g}; /* Ghidra pseudo-variable (split or register storage) */\n")
            elif g.startswith("XBE_FILE_HEADER_"):
                f.write(f"extern struct {{ unsigned int magic; }} {g}; /* the XBE image header, mapped at 0x10000 */\n")
            elif g.startswith("D3D8__"):
                f.write(f"extern unsigned int {g}; /* D3D8 render-state shadow variable */\n")
            elif g in pointer_globals:
                # The decompiler's own type for the label decides how its offsets scale: "G = (undefined1 *)..." means
                # G + 4 is 4 bytes on, "G = (undefined4 *)..." means G[1] is the next dword. Use the type it assigns
                # most; with no such cast, an indexed label is a dword array, anything else byte data.
                t = pointer_types.get(g) or ("undefined4 *" if g in indexed_globals else "undefined1 *")
                f.write(f"extern {t}{g}; /* dereferenced by the code */\n")
            elif g in wide_globals:
                f.write(f"extern {wide_globals[g]} {g}; /* holds an 8-byte value */\n")
            elif g in float_globals and g not in int_globals and label_width.get(g, 4) >= 4:
                f.write(f"extern float {g}; /* read as (float10)G: a float */\n")
            elif g in byte_globals or label_width.get(g, 4) == 1:
                f.write(f"extern undefined1 {g}; /* a byte */\n")
            elif label_width.get(g, 4) in (2, 3):
                f.write(f"extern undefined2 {g}; /* the next label is 2 bytes on */\n")
            else:
                f.write(f"extern undefined4 {g};\n")
    wanted = {(a, old) for a, ps in SOURCE_PATCHES.items() for old, _ in ps}
    print(f"source patches applied: {len(SOURCE_PATCHES_APPLIED)} of {len(wanted)}")
    for a, old in sorted(wanted - SOURCE_PATCHES_APPLIED):
        print(f"  not found (the export changed?): {a}: {old}")
    print(f"{len(pp_files)} files, {len(globals_used - defined)} global labels declared "
          f"({len(pointer_globals)} as pointers)")

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        # globals that end up pointers: ours (dereferenced somewhere) and those the database types as pointers
        typed_pointers = set(re.findall(r"\*\s*((?:_?DAT|PTR)_\w+)\s*;", program_text))
        casts = sum(pool.map(lambda p: fix_cast_errors(p, clang, [build, decomp], pointer_globals | typed_pointers),
                             pp_files))
    print("lines rewritten from clang error positions (casts, calls, pointer globals as numbers):", casts)

    results = {}
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for path, errors in pool.map(compile_file, [(clang, p, [build, decomp]) for p in pp_files]):
            results[path] = errors

    per_func = {}
    messages = collections.Counter()
    file_level = 0
    for path, errors in results.items():
        ranges = function_ranges(path)
        for _, addr, name in ranges:
            per_func[addr] = [name, 0, ""]
        for line, msg in errors:
            messages[re.sub(r"'[^']*'", "'X'", msg)] += 1
            owner = None
            for start, addr, name in ranges:
                if start <= line:
                    owner = addr
                else:
                    break
            if owner is None:
                file_level += 1
                continue
            per_func[owner][1] += 1
            if not per_func[owner][2]:
                per_func[owner][2] = msg[:160]

    # every error with its source line, for working through what's left
    with open(os.path.join(build, "compile_errors.txt"), "w", encoding="latin-1") as f:
        for path in sorted(results):
            if not results[path]:
                continue
            lines = open(path, encoding="latin-1").read().split("\n")
            for line, msg in results[path]:
                src = lines[line - 1].strip() if 0 < line <= len(lines) else ""
                f.write(f"{os.path.relpath(path, build)}:{line}: {msg}\n    {src[:200]}\n")

    with open(os.path.join(build, "compile_report.tsv"), "w", newline="") as f:
        w = csv.writer(f, delimiter="\t")
        w.writerow(["address", "name", "errors", "first_error"])
        for addr in sorted(per_func):
            w.writerow([addr, *per_func[addr]])

    tracker = os.path.join(decomp, "functions.tsv")
    rows = list(csv.DictReader(open(tracker), delimiter="\t"))
    for r in rows:
        info = per_func.get(r["address"].lower().zfill(8))
        if info is not None:
            r["status"] = "compiles" if info[1] == 0 else "errors"
    with open(tracker, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()), delimiter="\t")
        w.writeheader()
        w.writerows(rows)

    ok = sum(1 for v in per_func.values() if v[1] == 0)
    total_errors = sum(v[1] for v in per_func.values())
    print(f"functions: {len(per_func)}, compile cleanly: {ok} ({100 * ok / max(1, len(per_func)):.1f}%), "
          f"with errors: {len(per_func) - ok}; total errors {total_errors}, outside any function {file_level}")
    print("most common errors:")
    for m, c in messages.most_common(15):
        print(f"  {c:6}  {m}")


if __name__ == "__main__":
    main()
