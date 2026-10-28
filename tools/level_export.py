"""Assemble Digimon Rumble Arena 2 levels into one textured OBJ each, using the levl + node scene graph.

Usage: python level_export.py <level.res | levels_dir> <out_dir> [--textures=<surf_to_png.py output>] [--no-effects]

levl (one per level .res):
  +0x00 rel32 collision (tern), +0x04 rel32 scene root node; the other relocated pointers are static meshes
  (rdms) that are already in world space.
node (a Maya scene exported by the game's tools):
  +0x04 rel32 object pointer list (rel32 each, runs up to the first object)
  object: rel32 type string, rel32 name string, rel32 attribute block, u32, u32, u32 attribute count,
          u32, s32 parent object index (+0x1C, -1 = root), type-specific fields
  transform: attribute block = visibility, translate[3], rotate[3] (radians, xyz order), scale[3]
             (visibility 0 hides the subtree in the initial state);
             inline at +0x20: scalePivot, scalePivotTranslate, rotatePivot, rotatePivotTranslate (vec3 each)
  platform:  +0x24 rel32 geometry record (1 = none); record has a bbox[6] and, at +0x5C or +0x60, rel32 mesh (rdms)
  effect:    +0x28 rel32 name of another node in the same .res, instanced under this shape's transform
Geometry is right-handed Z-up; output OBJs are converted to Y-up like model_export.py.
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model_export  # noqa: E402
import res_unpack  # noqa: E402

NULL = 1
MAX_EFFECT_DEPTH = 4


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def translation(v):
    return [[1, 0, 0, v[0]], [0, 1, 0, v[1]], [0, 0, 1, v[2]], [0, 0, 0, 1]]


def scaling(v):
    return [[v[0], 0, 0, 0], [0, v[1], 0, 0], [0, 0, v[2], 0], [0, 0, 0, 1]]


def rotation_xyz(r):
    cx, sx, cy, sy, cz, sz = math.cos(r[0]), math.sin(r[0]), math.cos(r[1]), math.sin(r[1]), math.cos(r[2]), math.sin(r[2])
    rx = [[1, 0, 0, 0], [0, cx, -sx, 0], [0, sx, cx, 0], [0, 0, 0, 1]]
    ry = [[cy, 0, sy, 0], [0, 1, 0, 0], [-sy, 0, cy, 0], [0, 0, 0, 1]]
    rz = [[cz, -sz, 0, 0], [sz, cz, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
    return mat_mul(rz, mat_mul(ry, rx))  # Maya xyz order: X applied first


IDENTITY = translation((0, 0, 0))


def neg(v):
    return tuple(-x for x in v)


def maya_local(t, r, s, sp, spt, rp, rpt):
    """Maya transform matrix (column vectors): T * RPT * RP * R * RP^-1 * SPT * SP * S * SP^-1."""
    m = translation(t)
    for step in (translation(rpt), translation(rp), rotation_xyz(r), translation(neg(rp)),
                 translation(spt), translation(sp), scaling(s), translation(neg(sp))):
        m = mat_mul(m, step)
    return m


def apply(m, p, w=1.0):
    return tuple(m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] * w for i in range(3))


def det3(m):
    return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))


class Res:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        _, self.sections, self.entries = res_unpack.parse(self.data)
        self.base = self.sections[0][0]
        self.by_abs = {self.base + e["offset"]: e for e in self.entries if e["section"] == 0}
        self.by_name = {e["name"]: e for e in self.entries if e["name"]}

    def rel(self, at):
        return at + struct.unpack_from("<i", self.data, at)[0]

    def string(self, at):
        t = self.rel(at)
        return self.data[t:self.data.index(b"\0", t)].decode("latin-1")

    def target(self, at, tag):
        """Entry that the rel32 at `at` points to, if it has the given tag."""
        e = self.by_abs.get(self.rel(at))
        return e if e is not None and e["tag"] == tag else None


def node_objects(res, node):
    at = res.base + node["offset"]
    end = at + node["size"]
    list_at = res.rel(at + 4)
    objs, k = [], list_at
    while k < end and (not objs or k < objs[0]):
        o = res.rel(k)
        if not (at <= o < end):
            break
        objs.append(o)
        k += 4
    return objs


def collect_node(res, node, parent_world, out, depth=0, max_depth=MAX_EFFECT_DEPTH):
    """Append (rdms entry, world matrix) for every visible platform mesh in a node (and its effects)."""
    d = res.data
    objs = node_objects(res, node)
    worlds = {}  # index -> (world matrix, visible)

    def world(i):
        if i in worlds:
            return worlds[i]
        worlds[i] = (parent_world, True)  # guards against cycles
        o = objs[i]
        parent, = struct.unpack_from("<i", d, o + 0x1C)
        pw, pvis = world(parent) if 0 <= parent < len(objs) and parent != i else (parent_world, True)
        if res.string(o) == "transform":
            vals = struct.unpack_from("<10f", d, res.rel(o + 8))
            sp, spt, rp, rpt = (struct.unpack_from("<3f", d, o + 0x20 + 12 * n) for n in range(4))
            result = (mat_mul(pw, maya_local(vals[1:4], vals[4:7], vals[7:10], sp, spt, rp, rpt)), pvis and vals[0] != 0)
        else:
            result = (pw, pvis)
        worlds[i] = result
        return result

    for i, o in enumerate(objs):
        try:
            kind = res.string(o)
        except ValueError:
            continue  # list overrun past the last object
        if kind in ("platform", "effect") and not world(i)[1]:
            continue  # hidden (visibility 0) until gameplay shows it
        if kind == "platform":
            if struct.unpack_from("<i", d, o + 0x24)[0] == NULL:
                continue
            # The record's mesh pointer sits at +0x5C or +0x60 (some levels have an extra leading field).
            rec = res.rel(o + 0x24)
            mesh = next((m for k in range(0x50, 0x68, 4) if (m := res.target(rec + k, "rdms")) is not None), None)
            if mesh is not None:
                out.append((mesh, world(i)[0]))
        elif kind == "effect" and depth < max_depth:
            try:
                child = res.by_name.get(res.string(o + 0x28))
            except ValueError:
                child = None
            if child is not None and child["tag"] == "node" and child is not node:
                collect_node(res, child, world(i)[0], out, depth + 1, max_depth)


def level_meshes(res, effects=True):
    levl = next((e for e in res.entries if e["tag"] == "levl"), None)
    if levl is None:
        return []
    at = res.base + levl["offset"]
    toc, = struct.unpack_from("<I", res.data, 0x1C)
    relocs = struct.unpack_from(f"<{levl['relocs']}I", res.data, toc + 8 + 20 * levl["index"] + 16 + levl["reloc_info"])
    out = []
    for r in relocs:
        mesh = res.target(at + r, "rdms")
        if mesh is not None:
            out.append((mesh, IDENTITY))
    root = res.target(at + 4, "node")
    if root is not None:
        collect_node(res, root, IDENTITY, out, max_depth=MAX_EFFECT_DEPTH if effects else 0)
    return out


def export_level(path, res_rel, out_root, textures_root, effects=True):
    res = Res(path)
    placed = level_meshes(res, effects)
    if not placed:
        return 0
    surf_by_abs = {res.base + e["offset"]: e for e in res.entries if e["tag"] == "surf" and e["section"] == 0}
    out_path = os.path.join(out_root, res_rel + ".obj")
    out_dir = os.path.dirname(out_path)
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(out_path))[0]
    obj, mtl, materials = [f"mtllib {stem}.mtl"], [], {}
    base = 1
    for n, (mesh, m) in enumerate(placed):
        at = res.base + mesh["offset"]
        nverts, = struct.unpack_from("<I", res.data, at + 0x10)
        verts, tris = model_export.read_submesh(res.data, res.rel(at + 0x0C), nverts, model_export.RDMS_STRIDE)
        if not tris:
            continue
        if abs(det3(m)) < 1e-12:
            continue  # zero scale: hidden until animated
        tex = model_export.material_texture(res.data, res.rel(at + 4), surf_by_abs)
        key = tex["index"] if tex else None
        if key not in materials:
            name = f"m{len(materials)}"
            materials[key] = name
            png = model_export.texture_png(tex, res_rel, textures_root, out_dir)
            mtl += [f"newmtl {name}", "Kd 1 1 1"] + ([f"map_Kd {png}"] if png else []) + [""]
        verts = [(apply(m, p), apply(m, nm, 0.0), uv) for p, nm, uv in verts]
        verts = [(p, tuple(x / (math.sqrt(sum(c * c for c in nm)) or 1) for x in nm), uv) for p, nm, uv in verts]
        if det3(m) < 0:
            tris = [(a, c, b) for a, b, c in tris]  # mirrored transform flips winding
        obj.append(f"o {mesh['name'] or 'mesh'}_{mesh['index']:05d}")
        obj.append(f"usemtl {materials[key]}")
        obj += model_export.obj_part_lines(verts, tris, base)
        base += len(verts)
    with open(out_path, "w") as f:
        f.write("\n".join(obj) + "\n")
    with open(os.path.join(out_dir, stem + ".mtl"), "w") as f:
        f.write("\n".join(mtl))
    return len(placed)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        raise SystemExit(__doc__)
    src, out_root = args
    effects = "--no-effects" not in sys.argv
    textures_root = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--textures=")),
                         os.path.join(os.path.dirname(os.path.abspath(out_root)), "textures"))
    if os.path.isfile(src):
        targets = [src]
    else:
        targets = [os.path.join(src, f) for f in sorted(os.listdir(src)) if f.endswith(".res")]
    for path in targets:
        res_rel = "levels/" + os.path.splitext(os.path.basename(path))[0]
        try:
            n = export_level(path, res_rel, out_root, textures_root, effects)
            print(f"{os.path.basename(path)}: {n} placed meshes")
        except Exception as exc:
            print(f"{os.path.basename(path)}: FAILED {exc!r}")


if __name__ == "__main__":
    main()
