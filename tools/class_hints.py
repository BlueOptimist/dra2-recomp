"""Collect naming hints for recovered classes: strings and Lua config tables used by each class's functions.

Usage: python class_hints.py <decomp/functions.tsv> <analysis/function_strings.tsv> <analysis/lua_key_functions.tsv>
                             <analysis/classes.tsv> <out.tsv>

Name classes by adding rows to analysis/class_names.tsv, then run ClassFixups.java.
"""
import collections
import csv
import re
import sys


def main():
    funcs_path, strings_path, lua_path, classes_path, out_path = sys.argv[1:6]
    strings = {}
    for line in open(strings_path, encoding="latin-1").read().splitlines()[1:]:
        a, _, s = line.split("\t", 2)
        strings[int(a, 16)] = s.split("\x1f")
    lua = {}
    for r in csv.DictReader(open(lua_path), delimiter="\t"):
        lua[int(r["function"], 16)] = r["tables"]
    members = collections.defaultdict(list)
    for r in csv.DictReader(open(funcs_path), delimiter="\t"):
        if "::" in r["name"]:
            members[r["name"].split("::")[0]].append(int(r["address"], 16))
    classes = {r["class"]: r for r in csv.DictReader(open(classes_path), delimiter="\t")}
    interesting = re.compile(r"[A-Za-z]{3}")
    with open(out_path, "w", newline="") as f:
        w = csv.writer(f, delimiter="\t")
        w.writerow(["class", "functions", "size", "parents", "lua_tables", "strings"])
        rows = []
        for cls, addrs in members.items():
            strs = collections.Counter()
            tables = collections.Counter()
            for a in addrs:
                for s in strings.get(a, []):
                    if interesting.search(s) and len(s) < 120:
                        strs[s] += 1
                if a in lua:
                    for t in lua[a].split(","):
                        tables[t] += 1
            info = classes.get(cls, {})
            rows.append((cls, len(addrs), info.get("size", ""), info.get("parents", ""),
                         ",".join(t for t, _ in tables.most_common(3)),
                         " | ".join(s for s, _ in strs.most_common(6))))
        rows.sort(key=lambda r: -r[1])
        w.writerows(rows)
    hinted = sum(1 for r in rows if r[4] or r[5])
    print(f"{len(rows)} classes, {hinted} with string or Lua-table hints")


if __name__ == "__main__":
    main()
