"""Generate Lua 5.0 function names for analysis/function_names.tsv.

Usage: python lua_names.py <name_tables.tsv> <function_names.tsv>

Library functions come from the luaL_reg {name, function} tables found in .rdata (tools/ghidra_scripts/DumpRefs.java);
the table is classified by its member names. Core functions are named from error messages unique to one function in
the Lua 5.0 sources. Existing rows in function_names.tsv win; new rows are appended.
"""
import csv
import sys

TABLE_KINDS = [  # (marker names, prefix)
    ({"popen", "tmpfile", "input", "output"}, "lua_io_"),
    ({"seek", "__gc", "__tostring"}, "lua_file_"),
    ({"clock", "difftime", "getenv", "setlocale"}, "lua_os_"),
    ({"getinfo", "sethook", "traceback"}, "lua_db_"),
    ({"sin", "floor", "sqrt"}, "lua_math_"),
    ({"foreachi", "setn", "insert"}, "lua_table_"),
    ({"gsub", "format", "byte"}, "lua_str_"),
    ({"print", "pcall", "getmetatable"}, "luaB_"),
    ({"resume", "yield", "wrap"}, "lua_co_"),
]

# Core functions identified by an error message that occurs in exactly one Lua 5.0 function.
CORE = [
    ("001b2790", "luaL_where", "formats \"%s:%d: \" (lauxlib.c)"),
    ("001b29c0", "luaL_checkstack", "\"stack overflow (%s)\" (lauxlib.c)"),
    ("001b3460", "luaL_loadbuffer", "(L, buffer, size, name) on lua! resource text (lauxlib.c)"),
    ("001b3600", "luaL_argerror", "\"bad argument #%d to `%s' (%s)\" (lauxlib.c)"),
    ("001b89b0", "luaB_getmetatable", "\"__metatable\" (lbaselib.c)"),
    ("001b8a00", "luaB_setmetatable", "\"cannot change a protected metatable\" (lbaselib.c)"),
    ("001b9370", "getpath", "\"LUA_PATH\" (lbaselib.c require)"),
    ("001b96c0", "auxresume", "\"too many arguments to resume\" (lbaselib.c)"),
    ("001b9970", "base_open", "sets \"_G\" and \"_VERSION\" (lbaselib.c)"),
    ("001b9a30", "luaopen_base", "registers \"coroutine\" and \"_LOADED\" (lbaselib.c)"),
    ("001bb630", "luaD_growstack", "\"stack overflow\" (ldo.c)"),
    ("001bbb40", "luaD_call", "\"C stack overflow\" (ldo.c)"),
    ("001bbc80", "lua_yield", "\"attempt to yield across metamethod/C-call boundary\" (ldo.c)"),
    ("001bcc80", "lua_getinfo", "\"value for `lua_getinfo' is not a function\" (ldebug.c)"),
    ("001bcd40", "luaG_typeerror", "\"attempt to %s a %s value\" (ldebug.c)"),
    ("001bce30", "luaG_ordererror", "\"attempt to compare two %s values\" (ldebug.c)"),
    ("001be7c0", "luaV_execute", "\"`for' initial value must be a number\" (lvm.c)"),
    ("001c1b20", "read_numeral", "\"malformed number\" (llex.c)"),
    ("001c1f10", "luaX_checklimit", "\"too many %s (limit=%d)\" (llex.c)"),
    ("001c1f40", "inclinenumber", "\"lines in a chunk\" (llex.c)"),
    ("001c2240", "read_string", "\"unfinished string\" (llex.c)"),
    ("001c41c0", "primaryexp", "\"unexpected symbol\" (lparser.c)"),
    ("001c64c0", "LoadFunction", "\"bad nupvalues in %s\" (lundump.c)"),
    ("001c6560", "LoadConstants", "\"bad constant type (%d) in %s\" (lundump.c)"),
    ("001c69c0", "LoadHeader", "\"%s too new: read version\" (lundump.c)"),
    ("001c6b90", "luaU_undump", "\"binary string\" (lundump.c)"),
    ("001c71f0", "luaK_code", "\"code size overflow\" (lcode.c)"),
    ("001c6ff0", "addk", "\"constant table overflow\" (lcode.c)"),
]


def main():
    tables, names_path = sys.argv[1], sys.argv[2]
    rows = list(csv.DictReader(open(tables), delimiter="\t"))
    by_table = {}
    for r in rows:
        by_table.setdefault(r["table"], []).append(r)
    existing = {}
    with open(names_path) as f:
        for line in f.read().splitlines()[1:]:
            if line.strip():
                a, n, e = (line.split("\t") + ["", ""])[:3]
                existing[a.lower().zfill(8)] = n
    new = []
    taken = set(existing.values())

    def add(addr, name, evidence):
        addr = addr.lower().zfill(8)
        if addr in existing or name in taken:
            return
        existing[addr] = name
        taken.add(name)
        new.append((addr, name, evidence))

    for table, entries in by_table.items():
        members = {e["string"] for e in entries}
        prefix = next((p for markers, p in TABLE_KINDS if markers & members), None)
        if prefix is None:
            print("unclassified table", table, sorted(members)[:8])
            continue
        for e in entries:
            add(e["function"], prefix + e["string"].strip("_"), f"luaL_reg table at {table} entry \"{e['string']}\"")
    for addr, name, evidence in CORE:
        add(addr, name, evidence)
    with open(names_path, "a") as f:
        for addr, name, evidence in new:
            f.write(f"{addr}\t{name}\t{evidence}\n")
    print(f"appended {len(new)} names")


if __name__ == "__main__":
    main()
