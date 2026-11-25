#!/usr/bin/env python3
"""rgba2png.py IN.rgba WIDTH HEIGHT OUT.png: a raw RGBA frame (nano-buttonmap's) as a PNG, laid over
a grey checkerboard so the panel's translucent dim reads as it would over a game."""
import struct
import sys
import zlib


def main():
    src, w, h, dst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    data = open(src, 'rb').read()
    if len(data) != w * h * 4:
        sys.exit('%s: %d bytes, want %d' % (src, len(data), w * h * 4))
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            r, g, b, a = data[(y * w + x) * 4:(y * w + x) * 4 + 4]
            bg = 120 if ((x // 16) + (y // 16)) % 2 else 90
            rows += bytes((
                (r * a + bg * (255 - a)) // 255,
                (g * a + bg * (255 - a)) // 255,
                (b * a + bg * (255 - a)) // 255))

    def chunk(kind, body):
        c = kind + body
        return struct.pack('>I', len(body)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)


if __name__ == '__main__':
    main()
