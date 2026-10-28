"""Print the recovered class hierarchy (from analysis/classes.tsv) with current names and short hints.

Usage: python class_tree.py <classes.tsv> <class_names.tsv> <class_hints.tsv> [min_descendants]
"""
import collections
import csv
import sys


def main():
    classes = list(csv.DictReader(open(sys.argv[1]), delimiter="\t"))
    names = {r["class"]: r["name"] for r in csv.DictReader(open(sys.argv[2]), delimiter="\t")}
    hints = {r["class"]: r for r in csv.DictReader(open(sys.argv[3]), delimiter="\t")}
    min_desc = int(sys.argv[4]) if len(sys.argv) > 4 else 2
    children = collections.defaultdict(list)
    roots = []
    for c in classes:
        parents = [p for p in c["parents"].split(",") if p]
        if parents:
            children[parents[0]].append(c["class"])  # first parent only
        else:
            roots.append(c["class"])
    info = {c["class"]: c for c in classes}

    def count(c):
        return sum(1 + count(k) for k in children[c])

    def show(c, depth):
        n = names.get(c, c)
        h = hints.get(n) or hints.get(c) or {}
        hint = (h.get("lua_tables", "") + " " + h.get("strings", ""))[:70]
        print(f"{'  ' * depth}{n} [{c}] size={info.get(c, {}).get('size', '')} desc={count(c)} {hint}")
        for k in sorted(children[c], key=lambda x: -count(x)):
            if count(k) + 1 >= min_desc or depth < 1:
                show(k, depth + 1)

    for r in sorted(roots, key=lambda x: -count(x)):
        if count(r) >= min_desc:
            show(r, 0)


if __name__ == "__main__":
    main()
