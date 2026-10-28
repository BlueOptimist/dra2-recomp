"""Score classes by how much character-behaviour evidence their methods carry.

Usage: python find_character.py <decomp/functions.tsv> <analysis/function_strings.tsv> <analysis/lua_key_functions.tsv>
                                <unpacked_dir> <classes.tsv> <class_names.tsv>

Evidence per function:
  - animation names (every banm name in the character .res files, e.g. "idle", "run", "jump_up_loop")
  - movement/combat config keys read from characterConfig (walkSpeed, runSpeed, maxJumpCount, ...)
Functions are grouped by class (namespace in functions.tsv); free functions are grouped under "<free>".
"""
import collections
import csv
import json
import os
import sys

MOVEMENT_KEYS = {"walkSpeed", "runSpeed", "walkAnimSpeed", "runAnimSpeed", "maxJumpCount", "jumpHeightFactor",
                 "weight", "strength", "walkAccel", "turnSpeed", "grabThrowDelay", "reGrabDelay",
                 "escapeWindowFrames", "blendToRecoilTime", "crouchBlend", "blockBlend", "walkToRunBlend"}


def main():
    funcs_tsv, strings_tsv, lua_tsv, unpacked, classes_tsv, names_tsv = sys.argv[1:7]
    anims = set()
    for dp, _, fns in os.walk(os.path.join(unpacked, "characters")):
        if "_manifest.json" in fns:
            for e in json.load(open(os.path.join(dp, "_manifest.json")))["entries"]:
                if e["tag"] == "banm" and e["name"]:
                    anims.add(e["name"].split("/")[-1])
    print(f"{len(anims)} distinct animation names, e.g. {sorted(anims)[:8]}")

    owner = {}
    for r in csv.DictReader(open(funcs_tsv), delimiter="\t"):
        owner[int(r["address"], 16)] = r["name"].split("::")[0] if "::" in r["name"] else "<free>"
    anim_hits = collections.defaultdict(set)
    func_anims = {}
    for line in open(strings_tsv, encoding="latin-1").read().splitlines()[1:]:
        a, name, s = line.split("\t", 2)
        hit = anims & set(s.split("\x1f"))
        if hit:
            addr = int(a, 16)
            anim_hits[owner.get(addr, "<free>")] |= hit
            func_anims[addr] = (name, hit)
    key_hits = collections.defaultdict(set)
    for r in csv.DictReader(open(lua_tsv), delimiter="\t"):
        keys = MOVEMENT_KEYS & set(r["keys"].split(","))
        if keys:
            key_hits[owner.get(int(r["function"], 16), "<free>")] |= keys

    sizes = {}
    names = {r["class"]: r["name"] for r in csv.DictReader(open(names_tsv), delimiter="\t")}
    for r in csv.DictReader(open(classes_tsv), delimiter="\t"):
        sizes[names.get(r["class"], r["class"])] = r["size"]
    scores = collections.Counter()
    for c in set(anim_hits) | set(key_hits):
        scores[c] = len(anim_hits[c]) + 2 * len(key_hits[c])
    print("\nclass                         anims  move-keys  size      examples")
    for c, sc in scores.most_common(15):
        print(f"{c:30} {len(anim_hits[c]):5}  {len(key_hits[c]):9}  {sizes.get(c, ''):9} "
              f"{sorted(anim_hits[c])[:5]} {sorted(key_hits[c])[:4]}")
    free = sorted(((a, n, h) for a, (n, h) in func_anims.items() if owner.get(a) == "<free>"), key=lambda x: -len(x[2]))
    print("\nfree functions with the most animation names:")
    for a, n, h in free[:10]:
        print(f"  {a:08x} {n:28} {len(h):3} {sorted(h)[:6]}")


if __name__ == "__main__":
    main()
