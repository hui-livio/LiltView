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


def main():
    folder = Path(__file__).resolve().parent
    width, height = 96, 64
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
