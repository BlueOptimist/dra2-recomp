"""Export Digimon Rumble Arena 2 characters as rigged, animated glTF 2.0 (skeleton + skinned LOD0 mesh + every banm).

Usage: python anim_export.py <character.res | characters_dir> <out_dir> [--textures=<surf_to_png.py output>] [--fps=30]

body (skeleton), one per character .res:
  +0x04 rel32 bone array (0xA0 bytes per bone), +0x08 u32 mesh LOD count, +0x0C rel32 list of rel32 bmsh,
  +0x14 rel32 end of the bone array
  bone: rel32 name, u32 0, s16 parent (-1 = root), u8, u8,
        +0x10 quat rotate, +0x20 quat jointOrient, +0x30 quat rotateAxis (x, y, z, w floats),
        +0x40 scale[3], +0x50 translate[3], +0x5C f32, +0x60 inverse bind matrix (16 floats, row-vector layout)
  local = T * jointOrient * rotate * rotateAxis * S (Maya joint order; reproduces the inverse binds exactly)
bmsh skinning: vertex u8 slot[4] (paired with weight[4] in order) addresses banks of 13 matrices, where the
  first of each bank is reserved: palette index = slot - 1 - slot // 13 (verified by minimal mesh stretch)
banm (animation):
  +0x00 u32 track count, +0x04 rel32 track list, +0x08 u32 event count, +0x0C rel32 event list, +0x10 f32 duration (s)
  track: rel32 bone name, u16 n, u16 flags, u16 time[n] (1/60 s ticks, segment end times; the first starts at 0),
         then 3n+1 cubic Bezier control points (P0 C C P1 C C P2 ...), each = popcount(mask) s16 components
  flags: bits 0-3 component mask (x y z w), 4-7 stored count, 8-11 omitted count,
         12-15 channel: 0x0 rotate quaternion (/32767, omitted w rebuilt from unit length),
                        0x8 translate (/1024, omitted -> bind), 0x4 scale (/256, omitted -> 1)
Geometry and skeleton stay in the game's right-handed Z-up space under a root node rotated -90 deg about X (Y-up).
"""
import json
import math
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model_export  # noqa: E402
import res_unpack  # noqa: E402

TICKS_PER_SECOND = 60.0
ROT_SCALE, TRANS_SCALE, SCALE_SCALE = 1 / 32767, 1 / 1024, 1 / 256
CH_ROTATE, CH_TRANSLATE, CH_SCALE = 0x0, 0x8, 0x4
SLOT_BANK = 13
Z_UP_TO_Y_UP = [-math.sqrt(0.5), 0.0, 0.0, math.sqrt(0.5)]
LANG = re.compile(r"(deu|eng|esp|fra|ita|jpn)$")


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qnorm(q):
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / n for c in q)


class Res:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        _, self.sections, self.entries = res_unpack.parse(self.data)
        self.base = self.sections[0][0]
        self.by_abs = {self.base + e["offset"]: e for e in self.entries if e["section"] == 0}

    def rel(self, at):
        return at + struct.unpack_from("<i", self.data, at)[0]

    def string(self, at):
        t = self.rel(at)
        return self.data[t:self.data.index(b"\0", t)].decode("latin-1")


def read_skeleton(res, body):
    d, at = res.data, res.base + body["offset"]
    first, end = res.rel(at + 4), res.rel(at + 0x14)
    bones = []
    for o in range(first, end, 0xA0):
        bones.append({
            "name": res.string(o),
            "parent": struct.unpack_from("<h", d, o + 8)[0],
            "rotate": struct.unpack_from("<4f", d, o + 0x10),
            "orient": struct.unpack_from("<4f", d, o + 0x20),
            "axis": struct.unpack_from("<4f", d, o + 0x30),
            "scale": struct.unpack_from("<3f", d, o + 0x40),
            "translate": struct.unpack_from("<3f", d, o + 0x50),
            "inverse_bind": struct.unpack_from("<16f", d, o + 0x60),
        })
    nlods, = struct.unpack_from("<I", d, at + 8)
    lst = res.rel(at + 0x0C)
    lods = [res.by_abs.get(res.rel(lst + 4 * i)) for i in range(nlods)]
    return bones, [m for m in lods if m is not None and m["tag"] == "bmsh"]


def read_skinned_mesh(res, mesh):
    """Return [(positions, normals, uvs, joints, weights, triangles, texture_entry)] for a bmsh."""
    d, at = res.data, res.base + mesh["offset"]
    surf_by_abs = {res.base + e["offset"]: e for e in res.entries if e["tag"] == "surf" and e["section"] == 0}
    parts = []
    ngroups, = struct.unpack_from("<I", d, at)
    o = at + 0x0C
    for _ in range(ngroups):
        _, nbones = struct.unpack_from("<II", d, o)
        palette = struct.unpack_from(f"<{nbones}I", d, res.rel(o + 8))
        nsub, = struct.unpack_from("<I", d, o + 16)
        o += 20
        for _ in range(nsub):
            nverts, = struct.unpack_from("<I", d, o)
            desc = res.rel(o + 4)
            vtx = res.rel(desc + 0x28)
            pb_size, = struct.unpack_from("<I", d, desc + 0x18)
            tris = [t for t in model_export.push_buffer_triangles(d, res.rel(desc + 0x24), pb_size) if max(t) < nverts]
            pos, nrm, uv, jnt, wgt = [], [], [], [], []
            for i in range(nverts):
                v = vtx + i * model_export.BMSH_STRIDE
                pos.append(struct.unpack_from("<3f", d, v))
                w = struct.unpack_from("<4f", d, v + 12)
                nrm.append(struct.unpack_from("<3f", d, v + 28))
                uv.append(struct.unpack_from("<2f", d, v + 40))
                slots = struct.unpack_from("<4B", d, v + 56)
                # Skinning slots come in banks of 13: one reserved slot, then 12 palette entries.
                pal_idx = [s - 1 - s // SLOT_BANK for s in slots]
                js = [palette[p] if 0 <= p < len(palette) else 0 for p in pal_idx]
                total = sum(w) or 1.0
                jnt.append(js)
                wgt.append([x / total for x in w])
            mat_at = res.rel(o + 16)
            tex = model_export.material_texture(d, mat_at, surf_by_abs)
            layers, = struct.unpack_from("<I", d, mat_at + 0x58)
            # A DXT3 base texture is cut-out only in single-layer materials; with a second (reflection) layer
            # its alpha is the reflection mask (e.g. Agumon's shiny claws).
            cutout = tex is not None and layers == 1 and d[res.base + tex["offset"] + 8] == 0x0B
            parts.append((pos, nrm, uv, jnt, wgt, tris, tex, cutout))
            o += 20
    return parts


def bezier(p0, c0, c1, p1, u):
    a = 1 - u
    return tuple(a * a * a * x0 + 3 * a * a * u * x1 + 3 * a * u * u * x2 + u * u * u * x3
                 for x0, x1, x2, x3 in zip(p0, c0, c1, p1))


def read_track(res, t):
    d = res.data
    n, flags = struct.unpack_from("<HH", d, t + 4)
    times = [0.0] + [x / TICKS_PER_SECOND for x in struct.unpack_from(f"<{n}H", d, t + 8)]
    comps = [c for c in range(4) if flags >> c & 1]
    nvals = 3 * n + 1
    raw = struct.unpack_from(f"<{len(comps) * nvals}h", d, t + 8 + 2 * n) if comps else ()
    pts = [raw[i * len(comps):(i + 1) * len(comps)] for i in range(nvals)] if comps else []
    return {"bone": res.string(t), "channel": flags >> 12, "comps": comps, "times": times, "points": pts}


def sample_track(track, time):
    """Stored components at `time` (raw s16 units, evaluated on the Bezier curve)."""
    pts, times = track["points"], track["times"]
    if not pts:
        return ()
    if len(pts) == 1 or time <= 0:
        return pts[0]
    for i in range(len(times) - 1):
        t0, t1 = times[i], times[i + 1]
        if time <= t1:
            u = 0.0 if t1 <= t0 else (time - t0) / (t1 - t0)
            return bezier(pts[3 * i], pts[3 * i + 1], pts[3 * i + 2], pts[3 * i + 3], u)
    return pts[-1]


def full_value(track, raw, bone):
    ch, comps = track["channel"], track["comps"]
    if ch == CH_ROTATE:
        q = [0.0, 0.0, 0.0, None]
        for c, x in zip(comps, raw):
            q[c] = x * ROT_SCALE
        if q[3] is None:
            q[3] = math.sqrt(max(0.0, 1 - q[0] ** 2 - q[1] ** 2 - q[2] ** 2))
        return qnorm(q)
    if ch == CH_TRANSLATE:
        v = list(bone["translate"])
        for c, x in zip(comps, raw):
            v[c] = x * TRANS_SCALE
        return tuple(v)
    v = [1.0, 1.0, 1.0]
    for c, x in zip(comps, raw):
        v[c] = x * SCALE_SCALE
    return tuple(v)


def read_animation(res, e):
    d, at = res.data, res.base + e["offset"]
    ntracks, = struct.unpack_from("<I", d, at)
    lst = res.rel(at + 4)
    duration, = struct.unpack_from("<f", d, at + 0x10)
    tracks = [read_track(res, res.rel(lst + 4 * i)) for i in range(ntracks)]
    return duration, tracks


class GltfBuilder:
    def __init__(self):
        self.bin = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "DRA2 anim_export.py"}, "scene": 0, "scenes": [{"nodes": []}],
                    "nodes": [], "meshes": [], "materials": [], "textures": [], "images": [], "samplers": [{}],
                    "skins": [], "animations": [], "accessors": [], "bufferViews": [], "buffers": []}

    def accessor(self, values, fmt, gltf_type, component, target=None, minmax=False):
        while len(self.bin) % 4:
            self.bin.append(0)
        offset = len(self.bin)
        per = struct.calcsize("<" + fmt)
        for v in values:
            self.bin += struct.pack("<" + fmt, *(v if isinstance(v, (tuple, list)) else (v,)))
        view = {"buffer": 0, "byteOffset": offset, "byteLength": per * len(values)}
        if target:
            view["target"] = target
        self.doc["bufferViews"].append(view)
        acc = {"bufferView": len(self.doc["bufferViews"]) - 1, "componentType": component, "count": len(values), "type": gltf_type}
        if minmax:
            cols = list(zip(*[v if isinstance(v, (tuple, list)) else (v,) for v in values]))
            acc["min"], acc["max"] = [min(c) for c in cols], [max(c) for c in cols]
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1


def export_character(path, name, out_root, textures_root, res_rel, fps):
    res = Res(path)
    body = next((e for e in res.entries if e["tag"] == "body"), None)
    if body is None:
        return None
    bones, lods = read_skeleton(res, body)
    if not lods:
        return None
    g = GltfBuilder()
    nodes = g.doc["nodes"]
    out_dir = os.path.join(out_root)
    os.makedirs(out_dir, exist_ok=True)

    # Skeleton nodes 1..N under an axis-conversion root node 0.
    nodes.append({"name": name, "rotation": Z_UP_TO_Y_UP, "children": []})
    joint_nodes = []
    for b in bones:
        local_rot = qnorm(qmul(qmul(b["orient"], b["rotate"]), b["axis"]))
        nodes.append({"name": b["name"], "translation": list(b["translate"]), "rotation": list(local_rot),
                      "scale": list(b["scale"])})
        joint_nodes.append(len(nodes) - 1)
    for i, b in enumerate(bones):
        parent = nodes[joint_nodes[b["parent"]]] if 0 <= b["parent"] < len(bones) else nodes[0]
        parent.setdefault("children", []).append(joint_nodes[i])
    ibm = g.accessor([b["inverse_bind"] for b in bones], "16f", "MAT4", 5126)
    g.doc["skins"].append({"joints": joint_nodes, "inverseBindMatrices": ibm, "skeleton": 0})

    # Skinned LOD0 mesh.
    materials = {}
    prims = []
    for pos, nrm, uv, jnt, wgt, tris, tex, cutout in read_skinned_mesh(res, lods[0]):
        if not tris:
            continue
        key = (tex["index"] if tex else None, cutout)
        if key not in materials:
            mat = {"name": f"m{len(materials)}", "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 1.0}}
            png = model_export.texture_png(tex, res_rel, textures_root, out_dir)
            if png:
                g.doc["images"].append({"uri": png})
                g.doc["textures"].append({"source": len(g.doc["images"]) - 1, "sampler": 0})
                mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": len(g.doc["textures"]) - 1}
                if cutout:
                    mat["alphaMode"], mat["alphaCutoff"] = "MASK", 0.5
            g.doc["materials"].append(mat)
            materials[key] = len(g.doc["materials"]) - 1
        attrs = {"POSITION": g.accessor(pos, "3f", "VEC3", 5126, 34962, minmax=True),
                 "NORMAL": g.accessor(nrm, "3f", "VEC3", 5126, 34962),
                 "TEXCOORD_0": g.accessor(uv, "2f", "VEC2", 5126, 34962),
                 "JOINTS_0": g.accessor(jnt, "4H", "VEC4", 5123, 34962),
                 "WEIGHTS_0": g.accessor(wgt, "4f", "VEC4", 5126, 34962)}
        idx = g.accessor([i for t in tris for i in t], "I", "SCALAR", 5125, 34963)
        prims.append({"attributes": attrs, "indices": idx, "material": materials[key]})
    g.doc["meshes"].append({"name": name, "primitives": prims})
    nodes.append({"name": name + "_mesh", "mesh": 0, "skin": 0})
    g.doc["scenes"][0]["nodes"] = [0, len(nodes) - 1]

    # Animations, resampled to `fps` with linear interpolation.
    by_name = {b["name"]: i for i, b in enumerate(bones)}
    for e in res.entries:
        if e["tag"] != "banm":
            continue
        duration, tracks = read_animation(res, e)
        frames = max(2, int(round(duration * fps)) + 1)
        times = [min(duration, f / fps) for f in range(frames)]
        time_acc = g.accessor(times, "f", "SCALAR", 5126, minmax=True)
        anim = {"name": (e["name"] or f"anim_{e['index']}").split("/")[-1], "samplers": [], "channels": []}
        for tr in tracks:
            bi = by_name.get(tr["bone"])
            if bi is None:
                continue
            b = bones[bi]
            values = [full_value(tr, sample_track(tr, t), b) for t in times]
            if tr["channel"] == CH_ROTATE:
                path_name, fmt, typ = "rotation", "4f", "VEC4"
                values = [qnorm(qmul(qmul(b["orient"], q), b["axis"])) for q in values]
                for k in range(1, len(values)):  # keep consecutive quaternions in one hemisphere
                    if sum(a * c for a, c in zip(values[k - 1], values[k])) < 0:
                        values[k] = tuple(-c for c in values[k])
            elif tr["channel"] == CH_TRANSLATE:
                path_name, fmt, typ = "translation", "3f", "VEC3"
            elif tr["channel"] == CH_SCALE:
                path_name, fmt, typ = "scale", "3f", "VEC3"
            else:
                continue
            out_acc = g.accessor(values, fmt, typ, 5126)
            anim["samplers"].append({"input": time_acc, "output": out_acc, "interpolation": "LINEAR"})
            anim["channels"].append({"sampler": len(anim["samplers"]) - 1,
                                     "target": {"node": joint_nodes[bi], "path": path_name}})
        if anim["channels"]:
            g.doc["animations"].append(anim)

    bin_name = name + ".bin"
    g.doc["buffers"].append({"uri": bin_name, "byteLength": len(g.bin)})
    for key in ("textures", "images", "animations", "materials"):
        if not g.doc[key]:
            del g.doc[key]
    with open(os.path.join(out_dir, bin_name), "wb") as f:
        f.write(g.bin)
    with open(os.path.join(out_dir, name + ".gltf"), "w") as f:
        json.dump(g.doc, f, indent=1)
    return len(bones), len(g.doc.get("animations", []))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        raise SystemExit(__doc__)
    src, out_root = args
    textures_root = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--textures=")),
                         os.path.join(os.path.dirname(os.path.abspath(out_root)), "textures"))
    fps = next((float(a.split("=", 1)[1]) for a in sys.argv[1:] if a.startswith("--fps=")), 30.0)
    if os.path.isfile(src):
        files = [src]
    else:
        files = [os.path.join(src, f) for f in sorted(os.listdir(src)) if f.endswith(".res")]
    # One export per character: prefer the English variant of the per-language copies.
    chosen = {}
    for p in files:
        stem = os.path.splitext(os.path.basename(p))[0]
        base = LANG.sub("", stem)
        if base not in chosen or stem.endswith("eng"):
            chosen[base] = p
    for base, p in sorted(chosen.items()):
        res_rel = "characters/" + os.path.splitext(os.path.basename(p))[0]
        try:
            r = export_character(p, base, out_root, textures_root, res_rel, fps)
            print(f"{base}: " + (f"{r[0]} bones, {r[1]} animations" if r else "no skeleton"))
        except Exception as exc:
            print(f"{base}: FAILED {exc!r}")


if __name__ == "__main__":
    main()
