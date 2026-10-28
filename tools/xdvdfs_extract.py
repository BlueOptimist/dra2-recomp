"""Extract files from an original Xbox (XGD1) disc image's XDVDFS game partition.

Usage: python xdvdfs_extract.py <image.iso> <output_dir>
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets for redump-style images (XGD1/2/3) and plain game-partition images.
PARTITION_OFFSETS = [0x18300000, 0xFD90000, 0x2080000, 0]
ATTR_DIRECTORY = 0x10


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    raise SystemExit("No XDVDFS volume descriptor found")


def walk_dir(f, base, sector, size, out_dir, stats):
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    seen = set()

    def visit(offset):
        if offset in seen or offset + 14 > len(table):
            return
        seen.add(offset)
        left, right, start, length, attr, name_len = struct.unpack_from("<HHIIBB", table, offset)
        if left == 0xFFFF:  # padding, not an entry
            return
        name = table[offset + 14:offset + 14 + name_len].decode("latin-1")
        if left:
            visit(left * 4)
        path = os.path.join(out_dir, name)
        if attr & ATTR_DIRECTORY:
            os.makedirs(path, exist_ok=True)
            stats["dirs"] += 1
            walk_dir(f, base, start, length, path, stats)
        else:
            extract_file(f, base + start * SECTOR, length, path)
            stats["files"] += 1
            stats["bytes"] += length
        if right:
            visit(right * 4)

    visit(0)


def extract_file(f, offset, length, path):
    f.seek(offset)
    with open(path, "wb") as out:
        remaining = length
        while remaining:
            chunk = f.read(min(remaining, 8 * 1024 * 1024))
            if not chunk:
                raise SystemExit(f"Unexpected end of image while reading {path}")
            out.write(chunk)
            remaining -= len(chunk)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    image, out_dir = sys.argv[1], sys.argv[2]
    with open(image, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + len(MAGIC))
        root_sector, root_size = struct.unpack("<II", f.read(8))
        print(f"Game partition at 0x{base:X}, root dir sector {root_sector}, size {root_size}")
        os.makedirs(out_dir, exist_ok=True)
        stats = {"dirs": 0, "files": 0, "bytes": 0}
        walk_dir(f, base, root_sector, root_size, out_dir, stats)
    print(f"Extracted {stats['files']} files in {stats['dirs']} directories ({stats['bytes'] / 2**20:.1f} MiB)")


if __name__ == "__main__":
    main()
