"""Print the header, sections and linked XDK libraries of an original Xbox .xbe executable.

Usage: python xbe_info.py <default.xbe>
"""
import struct
import sys

# Entry point / kernel thunk XOR keys: (retail, debug)
ENTRY_KEYS = (0xA8FC57AB, 0x94859D4B)
THUNK_KEYS = (0x5B6D40B6, 0xEFB1F152)


def cstr(data, va, base):
    off = va - base
    return data[off:data.index(b"\0", off)].decode("latin-1")


def main():
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"XBEH":
        raise SystemExit("Not an XBE file")
    base, = struct.unpack_from("<I", data, 0x104)
    timestamp, = struct.unpack_from("<I", data, 0x114)
    cert_va, n_sect, sect_va = struct.unpack_from("<III", data, 0x118)
    entry_enc, = struct.unpack_from("<I", data, 0x128)
    thunk_enc, = struct.unpack_from("<I", data, 0x158)
    n_lib, lib_va = struct.unpack_from("<II", data, 0x160)

    for kind, ek, tk in zip(("retail", "debug"), ENTRY_KEYS, THUNK_KEYS):
        entry, thunk = entry_enc ^ ek, thunk_enc ^ tk
        if base <= entry < base + 0x10000000:
            break
    print(f"Base address : 0x{base:08X}")
    print(f"Build type   : {kind}")
    print(f"Entry point  : 0x{entry:08X}")
    print(f"Kernel thunk : 0x{thunk:08X}")
    print(f"Timestamp    : 0x{timestamp:08X}")

    c = cert_va - base
    title_id, = struct.unpack_from("<I", data, c + 8)
    title = data[c + 12:c + 12 + 80].decode("utf-16-le").split("\0")[0]
    print(f"Title        : {title}")
    print(f"Title ID     : {title_id:08X}")

    print("\nSections:")
    print(f"  {'name':<12}{'vaddr':>12}{'vsize':>10}{'raw off':>10}{'raw size':>10}  flags")
    for i in range(n_sect):
        o = sect_va - base + i * 56
        flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<IIIIII", data, o)
        print(f"  {cstr(data, name_va, base):<12}0x{va:08X}{vsize:>10X}{raw:>10X}{rsize:>10X}  {flags:#x}")

    print("\nLinked XDK libraries:")
    for i in range(n_lib):
        o = lib_va - base + i * 16
        name = data[o:o + 8].rstrip(b"\0").decode("latin-1")
        major, minor, build, flags = struct.unpack_from("<HHHH", data, o + 8)
        print(f"  {name:<10}{major}.{minor}.{build}")


if __name__ == "__main__":
    main()
