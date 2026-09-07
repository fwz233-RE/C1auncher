#!/usr/bin/env python3
"""Run the hardware-free GUI renderer and export dependency-free PNG fixtures."""
import argparse
from pathlib import Path
import struct
import subprocess
import zlib


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(width, height, pixels, scale):
    rows = []
    for y in range(height):
        row = b"\0" + b"".join(bytes([pixels[y * width + x]]) * scale for x in range(width))
        rows.extend([row] * scale)
    header = struct.pack(">IIBBBBB", width * scale, height * scale, 8, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b"")


parser = argparse.ArgumentParser()
parser.add_argument("--build-dir", type=Path, required=True)
args = parser.parse_args()
build = args.build_dir.resolve()
subprocess.run([str(build / "host-pkg-gui-tests"), str(build / "pkg-gui-preview.pbm")], check=True)
words = (build / "pkg-gui-preview.pbm").read_text(encoding="ascii").split()
assert words[:3] == ["P1", "296", "152"]
assert len(words[3:]) == 296 * 152
assert set(words[3:]) == {"0", "1"}
pixels = [0 if value == "1" else 255 for value in words[3:]]
(build / "pkg-gui-preview.png").write_bytes(png(296, 152, pixels, 1))
(build / "pkg-gui-preview-3x.png").write_bytes(png(296, 152, pixels, 3))
print("GUI framebuffer exported at native resolution and 3x nearest-neighbor scale.")
