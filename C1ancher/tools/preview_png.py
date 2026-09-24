"""Convert generated 296x152 P4 fixtures to nearest-neighbor PNGs (stdlib only)."""
import pathlib
import struct
import sys
import zlib


def chunk(kind, data):
    return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))


def convert(source):
    data = source.read_bytes()
    magic, size, bits = data.split(b'\n', 2)
    if magic != b'P4' or size != b'296 152' or len(bits) != 296 * 152 // 8:
        raise ValueError(f'Unexpected fixture: {source}')
    rows = bytearray()
    for y in range(152):
        row = bytearray([0])
        for x in range(296):
            color = 0 if bits[y * 37 + x // 8] & (0x80 >> (x % 8)) else 255
            row.extend([color] * 3)
        rows.extend(row * 3)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!IIBBBBB', 888, 456, 8, 0, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
    source.with_suffix('.png').write_bytes(png)


if __name__ == '__main__':
    for name in sys.argv[1:]:
        convert(pathlib.Path(name))
