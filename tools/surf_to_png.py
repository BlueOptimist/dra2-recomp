"""Convert Digimon Rumble Arena 2 'surf' textures (from res_unpack.py) to PNG. Pure Python, no dependencies.

Usage: python surf_to_png.py <file.surf | directory> <output_dir> [--jobs N]

surf layout (little-endian):
  0x00  u32 0, u32 id/hash
  0x08  u8 format (0x0A = DXT1, 0x0B = DXT3), u8 frame count, u8 1, u8 mip count
  0x0C  u16 width, u16 height
  0x10  u32 1
  0x14  frames * mips u32 pointers, frame-major, each relative to its own address
Data is plain (unswizzled) S3TC. Only mip 0 of each frame is exported; animated textures get _fNN suffixes.
"""
import os
import struct
import sys
import zlib
from multiprocessing import Pool

DXT1, DXT3 = 0x0A, 0x0B


def rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def color_palette(c0, c1, dxt1):
    p0, p1 = rgb565(c0), rgb565(c1)
    if c0 > c1 or not dxt1:
        p2 = tuple((2 * a + b) // 3 for a, b in zip(p0, p1))
        p3 = tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))
        return [p0 + (255,), p1 + (255,), p2 + (255,), p3 + (255,)]
    p2 = tuple((a + b) // 2 for a, b in zip(p0, p1))
    return [p0 + (255,), p1 + (255,), p2 + (255,), (0, 0, 0, 0)]


def decode(fmt, w, h, data):
    out = bytearray(w * h * 4)
    block = 8 if fmt == DXT1 else 16
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    cache = {}
    pos = 0
    for by in range(bh):
        for bx in range(bw):
            if fmt == DXT3:
                alpha = int.from_bytes(data[pos:pos + 8], "little")
                cpos = pos + 8
            else:
                alpha = None
                cpos = pos
            c0, c1, idx = struct.unpack_from("<HHI", data, cpos)
            key = (c0, c1)
            pal = cache.get(key)
            if pal is None:
                pal = cache[key] = color_palette(c0, c1, fmt == DXT1)
            for py in range(4):
                y = by * 4 + py
                if y >= h:
                    break
                row = (y * w + bx * 4) * 4
                for px in range(4):
                    if bx * 4 + px >= w:
                        break
                    i = py * 4 + px
                    r, g, b, a = pal[(idx >> (2 * i)) & 3]
                    if alpha is not None:
                        a = ((alpha >> (4 * i)) & 15) * 17
                    o = row + px * 4
                    out[o:o + 4] = bytes((r, g, b, a))
            pos += block
    return out


def write_png(path, w, h, rgba):
    stride = w * 4
    raw = b"".join(b"\0" + bytes(rgba[y * stride:(y + 1) * stride]) for y in range(h))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def mip0_size(fmt, w, h):
    return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if fmt == DXT1 else 16)


def convert(job):
    src, dest_base = job
    d = open(src, "rb").read()
    fmt, frames, _, mips = d[8], d[9], d[10], d[11]
    w, h = struct.unpack_from("<HH", d, 12)
    if fmt not in (DXT1, DXT3):
        return src, f"unknown format 0x{fmt:02X}"
    size = mip0_size(fmt, w, h)
    outputs = []
    for f in range(frames):
        field = 0x14 + 4 * f * mips
        start = field + struct.unpack_from("<I", d, field)[0]
        rgba = decode(fmt, w, h, d[start:start + size])
        dest = dest_base + (f"_f{f:02d}" if frames > 1 else "") + ".png"
        write_png(dest, w, h, rgba)
        outputs.append(dest)
    return src, None


def jobs_for(src, out_root):
    if os.path.isfile(src):
        name = os.path.basename(src)
        yield src, os.path.join(out_root, name.replace(".tif.surf", "").replace(".surf", ""))
        return
    for dp, _, fns in os.walk(src):
        for fn in fns:
            if fn.endswith(".surf"):
                rel = os.path.relpath(os.path.join(dp, fn), src)
                rel = rel.replace(".tif.surf", "").replace(".surf", "")
                yield os.path.join(dp, fn), os.path.join(out_root, rel)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--jobs")]
    jobs_n = next((int(a.split("=")[1]) for a in sys.argv[1:] if a.startswith("--jobs=")), os.cpu_count())
    if len(args) != 2:
        raise SystemExit(__doc__)
    work = list(jobs_for(*args))
    for _, dest in work:
        os.makedirs(os.path.dirname(dest), exist_ok=True)
    failures = []
    with Pool(jobs_n) as pool:
        for i, (src, err) in enumerate(pool.imap_unordered(convert, work, chunksize=8), 1):
            if err:
                failures.append((src, err))
            if i % 1000 == 0:
                print(f"{i}/{len(work)}", flush=True)
    print(f"Converted {len(work) - len(failures)}/{len(work)} textures")
    for src, err in failures[:20]:
        print(f"  FAILED {src}: {err}")


if __name__ == "__main__":
    main()
