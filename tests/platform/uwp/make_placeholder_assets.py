"""Regenerates the solid-color placeholder logos required by the UWP manifest.

Usage: python make_placeholder_assets.py <output-dir>
Uses only the standard library; the PNGs are original, trivial artifacts.
"""
import struct
import sys
import zlib

LOGOS = [("StoreLogo", 50), ("Square150x150Logo", 150), ("Square44x44Logo", 44)]
RGBA = bytes((0x33, 0x66, 0x99, 0xFF))


def chunk(kind: bytes, data: bytes) -> bytes:
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def solid_png(size: int) -> bytes:
    rows = b"".join(b"\x00" + RGBA * size for _ in range(size))
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main() -> None:
    out_dir = sys.argv[1]
    for name, size in LOGOS:
        with open(f"{out_dir}/{name}.png", "wb") as f:
            f.write(solid_png(size))


if __name__ == "__main__":
    main()
