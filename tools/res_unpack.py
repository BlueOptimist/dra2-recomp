"""Unpack Digimon Rumble Arena 2 (Xbox) .res containers.

Usage: python res_unpack.py <file.res | directory> <output_dir>

Layout (all little-endian, version 7):
  0x00  "res\\n"
  0x04  u32 version (7)
  0x08  u32 section 0 offset (0x1000)      0x0C  u32 section 0 size
  0x10  u32 section 1 offset               0x14  u32 section 1 size (bulk data, e.g. streamed audio)
  0x18  u32 offset of the indx chunk in section 0
  0x1C  u32 TOC offset                     0x20  u32 TOC size (entries + relocation table)
  0x24  u32 type count, then per type: 4-byte tag, u16 ?, u16 alignment
TOC:  u32 entry count, u32 ?, then 20-byte entries:
  tag[4], u32 offset (low 2 bits = section), u32 size, u32 reloc count, u32 reloc info
indx: u32 count, u32 ?, then 12-byte entries: s32 name ptr, tag[4], s32 data ptr
      (pointers are relative to the address of the pointer field itself; names live in strg)
lua!: u32 text length, u32 ?, then the Lua source text
musc: u16 format (1 = PCM), u16 channels, u32 sample rate, u32 data size, u32 data offset in section 1 (mdat)
"""
import json
import os
import re
import struct
import sys

MAGIC = b"res\n"
INVALID = re.compile(r'[<>:"|?*\x00-\x1f]')


def tag_str(tag):
    return tag.decode("latin-1").replace("!", "")


def parse(data):
    if data[:4] != MAGIC:
        raise ValueError("not a .res file")
    ver, s0_off, s0_size, s1_off, s1_size, indx_off, toc_off, toc_size, ntypes = struct.unpack_from("<9I", data, 4)
    sections = [(s0_off, s0_size), (s1_off, s1_size)]

    count, _ = struct.unpack_from("<II", data, toc_off)
    entries = []
    for i in range(count):
        tag, off, size, nrel, rel = struct.unpack_from("<4sIIII", data, toc_off + 8 + i * 20)
        sec = off & 3
        entries.append({"index": i, "tag": tag_str(tag), "section": sec, "offset": off & ~3,
                        "size": size, "relocs": nrel, "reloc_info": rel, "name": None})

    # Name entries through the indx chunk.
    by_offset = {(e["section"], e["offset"]): e for e in entries}
    indx = next((e for e in entries if e["tag"] == "indx"), None)
    if indx:
        base = s0_off + indx["offset"]
        n, _ = struct.unpack_from("<II", data, base)
        for i in range(n):
            p = base + 8 + i * 12
            name_rel, tag, data_rel = struct.unpack_from("<i4si", data, p)
            name_at = p + name_rel
            name = data[name_at:data.index(b"\0", name_at)].decode("latin-1")
            target = by_offset.get((0, p + 8 + data_rel - s0_off))
            if target is not None:
                target["name"] = name

    # Streamed music: a musc header points at its PCM data in section 1.
    for e in entries:
        if e["tag"] == "musc" and e["name"]:
            fmt, channels, rate, size, off = struct.unpack_from("<HHIII", data, s0_off + e["offset"])
            pcm = by_offset.get((1, off))
            if pcm is not None and pcm["name"] is None:
                pcm["name"] = e["name"]
                if fmt == 1:
                    pcm["wav"] = (channels, rate)
    return ver, sections, entries


def wav_header(channels, rate, size, bits=16):
    align = channels * bits // 8
    return (b"RIFF" + struct.pack("<I", 36 + size) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * align, align, bits) +
            b"data" + struct.pack("<I", size))


def payload(data, sections, e):
    start = sections[e["section"]][0] + e["offset"]
    blob = data[start:start + e["size"]]
    if e["tag"] == "lua":
        text_len, = struct.unpack_from("<I", blob, 0)
        if 8 + text_len <= len(blob):
            return blob[8:8 + text_len], "lua"
    if "wav" in e:
        return wav_header(*e["wav"], len(blob)) + blob, "wav"
    return blob, e["tag"]


def unpack_file(path, out_dir):
    with open(path, "rb") as f:
        data = f.read()
    ver, sections, entries = parse(data)
    os.makedirs(out_dir, exist_ok=True)
    used = set()
    for e in entries:
        if e["tag"] in ("indx", "strg"):
            continue
        blob, ext = payload(data, sections, e)
        if e["name"]:
            rel = INVALID.sub("_", e["name"]).replace("\\", "/").strip("/")
        else:
            rel = f"_unnamed/{e['index']:05d}"
        rel = f"{rel}.{ext}"
        if rel in used:
            rel = f"{rel[:-len(ext) - 1]}_{e['index']:05d}.{ext}"
        used.add(rel)
        e["file"] = rel
        dest = os.path.join(out_dir, *rel.split("/"))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as out:
            out.write(blob)
    with open(os.path.join(out_dir, "_manifest.json"), "w") as m:
        json.dump({"version": ver, "sections": sections, "entries": entries}, m, indent=1)
    return entries


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, out_root = sys.argv[1], sys.argv[2]
    if os.path.isfile(src):
        targets = [(src, os.path.join(out_root, os.path.splitext(os.path.basename(src))[0]))]
    else:
        targets = []
        for dp, _, fns in os.walk(src):
            for fn in sorted(fns):
                if fn.endswith(".res"):
                    rel = os.path.relpath(os.path.join(dp, fn), src)
                    targets.append((os.path.join(dp, fn), os.path.join(out_root, os.path.splitext(rel)[0])))
    total = named = 0
    failures = []
    for path, out_dir in targets:
        try:
            entries = unpack_file(path, out_dir)
        except Exception as exc:  # keep going; report at the end
            failures.append((path, repr(exc)))
            continue
        content = [e for e in entries if e["tag"] not in ("indx", "strg")]
        total += len(content)
        named += sum(1 for e in content if e["name"])
    print(f"Unpacked {len(targets) - len(failures)}/{len(targets)} files: {total} entries, {named} named")
    for path, err in failures:
        print(f"  FAILED {path}: {err}")


if __name__ == "__main__":
    main()
