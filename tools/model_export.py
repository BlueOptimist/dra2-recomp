"""Export Digimon Rumble Arena 2 meshes (bmsh = skinned, rdms = static) from .res files to Wavefront OBJ + MTL.

Usage: python model_export.py <file.res | extracted_dir> <models_dir> [--textures=<dir of surf_to_png.py output>]

Works on the .res containers directly because meshes reference their materials (gshd) and textures (surf)
through pointers relative to section 0, i.e. across resources.

bmsh (skinned mesh):
  0x00 u32 group count, u32 8, f32 1/4096, then per group (20 bytes):
       u32 1, u32 bone count, rel32 bone palette (u32 bone ids), u32 0, u32 submesh count,
       then per submesh (20 bytes): u32 vertex count, rel32 descriptor, f32 radius, u32 0, rel32 material (gshd)
  vertex, 60 bytes: f32 pos[3], f32 weight[4], f32 normal[3], f32 uv[2], u32 0, u32 0, u8 palette index[4]
rdms (static mesh):
  0x00 u32 1, rel32 material (gshd), u32 1, rel32 descriptor, u32 vertex count, u32 0, u32 1, f32 radius, ...
  vertex, 44 bytes: f32 pos[3], f32 normal[3], u32 ARGB color, f32 uv[2], u32 0, u32 0
descriptor: +0x18 u32 push buffer size, +0x24 rel32 push buffer, +0x28 rel32 vertex data
push buffer: NV2A commands, SET_BEGIN_END (0x17FC) primitive + ARRAY_ELEMENT16/32 (0x1800/0x1808) indices
gshd (material): +0x58 u32 texture count, +0x60 + 0x78*i rel32 texture (surf)
rel32 = signed offset from the address of the field itself. All values little-endian.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import res_unpack  # noqa: E402

BEGIN_END, ARRAY_ELEMENT16, ARRAY_ELEMENT32 = 0x17FC, 0x1800, 0x1808
PRIM_TRIANGLES, PRIM_STRIP, PRIM_FAN, PRIM_QUADS = 5, 6, 7, 8
BMSH_STRIDE, RDMS_STRIDE = 60, 44


def rel(data, at):
    return at + struct.unpack_from("<i", data, at)[0]


def push_buffer_triangles(data, at, size):
    """Walk an NV2A push buffer and return triangles as index triples (D3D winding)."""
    tris = []
    prim, idx = None, []
    end = at + size
    while at + 4 <= end:
        word, = struct.unpack_from("<I", data, at)
        method, count = word & 0x1FFC, (word >> 18) & 0x7FF
        args = struct.unpack_from(f"<{count}I", data, at + 4)
        at += 4 + 4 * count
        if method == BEGIN_END:
            if args[0] == 0 and prim is not None:
                tris.extend(assemble(prim, idx))
                prim, idx = None, []
            else:
                prim = args[0]
        elif method == ARRAY_ELEMENT16:
            for a in args:
                idx.extend((a & 0xFFFF, a >> 16))
        elif method == ARRAY_ELEMENT32:
            idx.extend(a & 0xFFFF for a in args)
        elif word == 0:
            break
    return tris


def assemble(prim, idx):
    out = []
    if prim == PRIM_TRIANGLES:
        out = [tuple(idx[i:i + 3]) for i in range(0, len(idx) - 2, 3)]
    elif prim == PRIM_STRIP:
        for i in range(len(idx) - 2):
            a, b, c = idx[i:i + 3]
            out.append((a, b, c) if i % 2 == 0 else (b, a, c))
    elif prim == PRIM_FAN:
        out = [(idx[0], idx[i], idx[i + 1]) for i in range(1, len(idx) - 1)]
    elif prim == PRIM_QUADS:
        for i in range(0, len(idx) - 3, 4):
            a, b, c, d = idx[i:i + 4]
            out += [(a, b, c), (a, c, d)]
    return [t for t in out if t[0] != t[1] and t[1] != t[2] and t[0] != t[2]]


def read_submesh(data, desc_at, nverts, stride):
    pb_at, vtx_at = rel(data, desc_at + 0x24), rel(data, desc_at + 0x28)
    pb_size, = struct.unpack_from("<I", data, desc_at + 0x18)
    tris = push_buffer_triangles(data, pb_at, pb_size)
    verts = []
    for i in range(nverts):
        o = vtx_at + i * stride
        if stride == BMSH_STRIDE:
            pos = struct.unpack_from("<3f", data, o)
            nrm = struct.unpack_from("<3f", data, o + 28)
            uv = struct.unpack_from("<2f", data, o + 40)
        else:
            pos = struct.unpack_from("<3f", data, o)
            nrm = struct.unpack_from("<3f", data, o + 12)
            uv = struct.unpack_from("<2f", data, o + 28)
        verts.append((pos, nrm, uv))
    tris = [t for t in tris if max(t) < nverts]
    return verts, tris


def material_texture(data, mat_at, surf_by_abs):
    """Return the TOC entry of the first texture of a gshd material, or None."""
    count, = struct.unpack_from("<I", data, mat_at + 0x58)
    if count == 0:
        return None
    return surf_by_abs.get(rel(data, mat_at + 0x60))


def meshes_in(data, sections, entries):
    """Yield (entry, [(verts, tris, texture_entry)]) for every bmsh/rdms entry."""
    base = sections[0][0]
    surf_by_abs = {base + e["offset"]: e for e in entries if e["tag"] == "surf" and e["section"] == 0}
    for e in entries:
        if e["tag"] not in ("bmsh", "rdms") or e["section"] != 0:
            continue
        at = base + e["offset"]
        parts = []
        if e["tag"] == "bmsh":
            ngroups, = struct.unpack_from("<I", data, at)
            o = at + 0x0C
            for _ in range(ngroups):
                nsub, = struct.unpack_from("<I", data, o + 16)
                o += 20
                for _ in range(nsub):
                    nverts, = struct.unpack_from("<I", data, o)
                    verts, tris = read_submesh(data, rel(data, o + 4), nverts, BMSH_STRIDE)
                    parts.append((verts, tris, material_texture(data, rel(data, o + 16), surf_by_abs)))
                    o += 20
        else:
            nverts, = struct.unpack_from("<I", data, at + 0x10)
            verts, tris = read_submesh(data, rel(data, at + 0x0C), nverts, RDMS_STRIDE)
            parts.append((verts, tris, material_texture(data, rel(data, at + 4), surf_by_abs)))
        yield e, parts


def texture_png(tex, res_rel, textures_root, out_dir):
    """Relative path from out_dir to the PNG that surf_to_png.py produced for this texture entry."""
    if tex is None:
        return None
    # Same naming as res_unpack.unpack_file (no duplicate-name suffixes; those are rare for textures).
    name = res_unpack.INVALID.sub("_", tex["name"]).strip("/") if tex["name"] else f"_unnamed/{tex['index']:05d}"
    name = name.replace(".tif", "")
    png = os.path.join(textures_root, res_rel, *name.split("/")) + ".png"
    if not os.path.exists(png):
        png = png[:-4] + "_f00.png"
    return os.path.relpath(png, out_dir).replace("\\", "/")


def to_obj_axes(v):
    """Game data is right-handed Z-up (Maya); OBJ convention is right-handed Y-up."""
    return v[0], v[2], -v[1]


def obj_part_lines(verts, tris, base):
    """v/vt/vn/f lines for one part. Stored winding is already counter-clockwise front-facing once axes are converted."""
    lines = ["v {:.6f} {:.6f} {:.6f}".format(*to_obj_axes(p)) for p, _, _ in verts]
    lines += [f"vt {uv[0]:.6f} {1 - uv[1]:.6f}" for _, _, uv in verts]  # OBJ UV origin is bottom-left
    lines += ["vn {:.6f} {:.6f} {:.6f}".format(*to_obj_axes(nm)) for _, nm, _ in verts]
    lines += [f"f {a + base}/{a + base}/{a + base} {b + base}/{b + base}/{b + base} {c + base}/{c + base}/{c + base}"
              for a, b, c in tris]
    return lines


def write_obj(path, parts, res_rel, textures_root):
    out_dir = os.path.dirname(path)
    stem = os.path.splitext(os.path.basename(path))[0]
    mtl_lines, obj_lines = [], [f"mtllib {stem}.mtl"]
    base = 1
    for n, (verts, tris, tex) in enumerate(parts):
        mat = f"{stem}_m{n}"
        png = texture_png(tex, res_rel, textures_root, out_dir)
        mtl_lines += [f"newmtl {mat}", "Kd 1 1 1"] + ([f"map_Kd {png}"] if png else []) + [""]
        obj_lines.append(f"o part{n}")
        obj_lines.append(f"usemtl {mat}")
        obj_lines += obj_part_lines(verts, tris, base)
        base += len(verts)
    with open(path, "w") as f:
        f.write("\n".join(obj_lines) + "\n")
    with open(os.path.join(out_dir, stem + ".mtl"), "w") as f:
        f.write("\n".join(mtl_lines))


def export_res(path, res_rel, models_root, textures_root):
    data = open(path, "rb").read()
    _, sections, entries = res_unpack.parse(data)
    count = 0
    for e, parts in meshes_in(data, sections, entries):
        if not any(tris for _, tris, _ in parts):
            continue
        name = e["name"] or f"{e['index']:05d}"
        out = os.path.join(models_root, res_rel, *name.split("/")) + f"_{e['tag']}.obj"
        os.makedirs(os.path.dirname(out), exist_ok=True)
        write_obj(out, parts, res_rel, textures_root)
        count += 1
    return count


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        raise SystemExit(__doc__)
    src, models_root = args
    textures_root = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--textures=")),
                         os.path.join(os.path.dirname(os.path.abspath(models_root)), "textures"))
    if os.path.isfile(src):
        # Keep the path below 'extracted' so texture links match surf_to_png.py's directory layout.
        full = os.path.abspath(src)
        parts = full.split(os.sep)
        root = os.sep.join(parts[:len(parts) - parts[::-1].index("extracted")]) if "extracted" in parts else os.path.dirname(full)
        targets = [(src, os.path.splitext(os.path.relpath(full, root))[0])]
    else:
        targets = [(os.path.join(dp, fn), os.path.splitext(os.path.relpath(os.path.join(dp, fn), src))[0])
                   for dp, _, fns in os.walk(src) for fn in sorted(fns) if fn.endswith(".res")]
    total, failures = 0, []
    for path, res_rel in targets:
        try:
            total += export_res(path, res_rel, models_root, textures_root)
        except Exception as exc:
            failures.append((path, repr(exc)))
    print(f"Exported {total} meshes from {len(targets) - len(failures)}/{len(targets)} .res files")
    for path, err in failures[:20]:
        print(f"  FAILED {path}: {err}")


if __name__ == "__main__":
    main()
