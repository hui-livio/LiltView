#!/usr/bin/env python3
"""Regenerate original synthetic fixtures. WebP encoding uses local libwebp.

SPDX-License-Identifier: Apache-2.0
Tests consume checked-in files and do not need this generator or a system encoder.
"""
import binascii
import ctypes
import ctypes.util
from pathlib import Path
import struct
import zlib


def gif_tiff(folder, width, height):
    # Original two-frame GIF fixture: first red, second blue. Reset LZW before
    # each pixel to keep this tiny generator independent of an encoder library.
    header = b"GIF89a" + struct.pack("<HHBBB", width, height, 0x80, 0, 0) + bytes((255, 0, 0, 0, 0, 255))
    frames = bytearray()
    for index in (0, 1):
        frames += b"\x21\xf9\x04\x04\x0a\x00\x00\x00"
        frames += b"\x2c" + struct.pack("<HHHHB", 0, 0, width, height, 0) + b"\x02"
        codes = [code for _ in range(width * height) for code in (4, index)] + [5]
        stream, bits, count = bytearray(), 0, 0
        for code in codes:
            bits |= code << count
            count += 3
            while count >= 8:
                stream.append(bits & 255)
                bits >>= 8
                count -= 8
        if count:
            stream.append(bits & 255)
        for offset in range(0, len(stream), 255):
            block = stream[offset:offset + 255]
            frames += bytes((len(block),)) + block
        frames += b"\x00"
    (folder / "red-first-frame.gif").write_bytes(header + frames + b";")
    loop = b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00"
    (folder / "red-blue-loop.gif").write_bytes(header + loop + frames + b";")

    # Classic little-endian TIFF with one uncompressed RGB strip.
    tags = [(256, 4, 1, width), (257, 4, 1, height), (258, 3, 3, 0),
            (259, 3, 1, 1), (262, 3, 1, 2), (273, 4, 1, 0), (274, 3, 1, 1),
            (277, 3, 1, 3), (278, 4, 1, height), (279, 4, 1, width * height * 3), (284, 3, 1, 1)]
    bits_offset = 8 + 2 + len(tags) * 12 + 4
    pixel_offset = bits_offset + 6
    tags[2] = (258, 3, 3, bits_offset)
    tags[5] = (273, 4, 1, pixel_offset)
    entries = b"".join(struct.pack("<HHII", *tag) for tag in tags)
    data = b"II\x2a\x00\x08\x00\x00\x00" + struct.pack("<H", len(tags)) + entries
    data += bytes(4) + struct.pack("<HHH", 8, 8, 8) + bytes((255, 0, 0)) * width * height
    (folder / "red.tiff").write_bytes(data)


def main():
    folder = Path(__file__).resolve().parent
    width, height = 96, 64
    gif_tiff(folder, width, height)
    rgba = bytes((255, 0, 0, 128)) * width * height

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", binascii.crc32(kind + payload))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))))
    png += chunk(b"IEND", b"")
    (folder / "red-alpha.png").write_bytes(png)

    bgr = bytes((0, 0, 255)) * width * height
    bmp = b"BM" + struct.pack("<IHHI", 54 + len(bgr), 0, 0, 54)
    bmp += struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(bgr), 0, 0, 0, 0) + bgr
    (folder / "red.bmp").write_bytes(bmp)
    (folder / "red-alpha.svg").write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">'
        f'<rect width="{width}" height="{height}" fill="red" fill-opacity="0.5"/></svg>\n'
    )

    library = ctypes.CDLL(ctypes.util.find_library("webp"))
    library.WebPEncodeLosslessRGBA.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                              ctypes.POINTER(ctypes.c_void_p)]
    library.WebPEncodeLosslessRGBA.restype = ctypes.c_size_t
    library.WebPFree.argtypes = [ctypes.c_void_p]
    output = ctypes.c_void_p()
    size = library.WebPEncodeLosslessRGBA(rgba, width, height, width * 4, ctypes.byref(output))
    if not size:
        raise RuntimeError("libwebp fixture encoding failed")
    try:
        (folder / "red-alpha.webp").write_bytes(ctypes.string_at(output, size))
    finally:
        library.WebPFree(output)


if __name__ == "__main__":
    main()
