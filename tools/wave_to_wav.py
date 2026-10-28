"""Convert Digimon Rumble Arena 2 'wave' sound effects (from res_unpack.py) to 16-bit PCM .wav. Pure Python.

Usage: python wave_to_wav.py <file.wave | directory> <output_dir> [--jobs=N]

wave layout (little-endian):
  0x00  u32 id/hash, u8[4] flags (05 0x 01 00; byte 5 = channel count)
  0x08  u32 pointer to WAVEFORMATEX (relative to 0x08, always -> 0x20)
  0x0C  u32 data size
  0x10  u32 pointer to sample data (relative to 0x10, -> 0x34); data size 0 / pointer 1 = empty sound
  0x20  WAVEFORMATEX: tag 0x69 (Xbox ADPCM), channels, rate, avg bytes/s, block align 36*ch, 4 bits, cb 2,
        samples per block 64
Xbox ADPCM block, per channel: s16 predictor, u8 step index, u8 pad; then 32 bytes of 4-bit IMA codes,
interleaved between channels in 4-byte words (low nibble first). Each block decodes to 65 samples per channel:
the header predictor is a real sample, followed by the 64 coded ones. Measured: without the header sample every
block boundary shows a double-sized step; with it, boundaries are as smooth as the block interior.
"""
import os
import struct
import sys
from multiprocessing import Pool

STEPS = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767,
]
INDEX_ADJUST = [-1, -1, -1, -1, 2, 4, 6, 8] * 2
XBOX_ADPCM = 0x69


def decode_xbox_adpcm(data, channels):
    block = 36 * channels
    out = [[] for _ in range(channels)]
    for b in range(0, len(data) - block + 1, block):
        state = []
        for c in range(channels):
            pred, index = struct.unpack_from("<hB", data, b + 4 * c)
            state.append([pred, min(index, 88)])
            out[c].append(pred)
        pos = b + 4 * channels
        for _ in range(8):
            for c in range(channels):
                pred, index = state[c]
                dest = out[c]
                for byte in data[pos:pos + 4]:
                    for code in (byte & 15, byte >> 4):
                        step = STEPS[index]
                        diff = step >> 3
                        if code & 4:
                            diff += step
                        if code & 2:
                            diff += step >> 1
                        if code & 1:
                            diff += step >> 2
                        pred = pred - diff if code & 8 else pred + diff
                        pred = -32768 if pred < -32768 else 32767 if pred > 32767 else pred
                        index += INDEX_ADJUST[code]
                        index = 0 if index < 0 else 88 if index > 88 else index
                        dest.append(pred)
                state[c] = [pred, index]
                pos += 4
    if channels == 1:
        return struct.pack(f"<{len(out[0])}h", *out[0])
    inter = [s for frame in zip(*out) for s in frame]
    return struct.pack(f"<{len(inter)}h", *inter)


def wav_bytes(channels, rate, pcm):
    align = channels * 2
    return (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * align, align, 16) +
            b"data" + struct.pack("<I", len(pcm)) + pcm)


def convert(job):
    src, dest = job
    d = open(src, "rb").read()
    fmt_at = 8 + struct.unpack_from("<I", d, 8)[0]
    size, = struct.unpack_from("<I", d, 12)
    data_at = 16 + struct.unpack_from("<I", d, 16)[0]
    tag, channels, rate = struct.unpack_from("<HHI", d, fmt_at)
    if tag != XBOX_ADPCM:
        return src, f"unknown format tag 0x{tag:X}"
    pcm = decode_xbox_adpcm(d[data_at:data_at + size], channels) if size else b""
    with open(dest, "wb") as f:
        f.write(wav_bytes(channels, rate, pcm))
    return src, None


def jobs_for(src, out_root):
    if os.path.isfile(src):
        yield src, os.path.join(out_root, os.path.basename(src)[:-5] + ".wav")
        return
    for dp, _, fns in os.walk(src):
        for fn in fns:
            if fn.endswith(".wave"):
                rel = os.path.relpath(os.path.join(dp, fn), src)
                yield os.path.join(dp, fn), os.path.join(out_root, rel[:-5] + ".wav")


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
    print(f"Converted {len(work) - len(failures)}/{len(work)} sounds")
    for src, err in failures[:20]:
        print(f"  FAILED {src}: {err}")


if __name__ == "__main__":
    main()
