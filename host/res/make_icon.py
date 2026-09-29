"""Renders the AOI icon (violet->cyan rounded square with a waveform) to app.ico.

Pure Python, no imaging libraries: 4x4 supersampled coverage, written as
PNG-compressed ICO entries (Vista+ reads those at every size).
"""
import struct
import zlib
from pathlib import Path


def lerp(a, b, t):
    return a + (b - a) * t


def render(size):
    ss = 4
    violet, cyan = (139, 108, 255), (34, 211, 238)
    bars = [0.36, 0.62, 0.84, 0.56, 0.38]
    rows = []
    for y in range(size):
        row = bytearray([0])  # PNG filter byte
        for x in range(size):
            cov = bar = 0
            for sy in range(ss):
                for sx in range(ss):
                    u = (x + (sx + 0.5) / ss) / size
                    v = (y + (sy + 0.5) / ss) / size
                    # rounded square, radius 22 %
                    r = 0.22
                    dx = max(r - u, 0, u - (1 - r))
                    dy = max(r - v, 0, v - (1 - r))
                    if dx * dx + dy * dy <= r * r:
                        cov += 1
                        # waveform bars
                        for i, h in enumerate(bars):
                            cx = 0.26 + i * 0.12
                            if abs(u - cx) < 0.036 and abs(v - 0.5) < h / 2:
                                bar += 1
                                break
            n = ss * ss
            t = (x + y) / (2 * size)
            base = [lerp(violet[k], cyan[k], t) for k in range(3)]
            w = bar / max(cov, 1)
            col = [round(lerp(base[k], 255, w)) for k in range(3)]
            row += bytes(col + [round(255 * cov / n)])
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    sizes = [16, 24, 32, 48, 64, 256]
    images = [render(s) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, img in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    out = Path(__file__).with_name("app.ico")
    out.write_bytes(header + entries + b"".join(images))
    print(f"wrote {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
