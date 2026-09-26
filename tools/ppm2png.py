#!/usr/bin/env python3
"""PPM(P6) -> PNG 转换，只用标准库 zlib，避免额外依赖。

用法: python tools/ppm2png.py <in.ppm> <out.png>
"""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # 解析 header: P6 <ws> w <ws> h <ws> maxval <single ws>
    pos = 0

    def token():
        nonlocal pos
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if pos < len(data) and data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            return token()
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        return data[start:pos]

    magic = token()
    if magic != b"P6":
        raise ValueError("只支持 P6 格式，收到 %r" % magic)
    w = int(token())
    h = int(token())
    maxval = int(token())
    pos += 1   # 跳过单个空白字符
    if maxval != 255:
        raise ValueError("只支持 maxval=255")
    pixels = data[pos:pos + w * h * 3]
    if len(pixels) != w * h * 3:
        raise ValueError("像素数据长度不符")
    return w, h, pixels


def write_png(path, w, h, rgb):
    def chunk(tag, payload):
        out = struct.pack(">I", len(payload)) + tag + payload
        out += struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        return out

    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)                      # filter type 0
        raw.extend(rgb[y * stride:(y + 1) * stride])

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    w, h, rgb = read_ppm(sys.argv[1])
    write_png(sys.argv[2], w, h, rgb)
    print("已转换 %dx%d -> %s" % (w, h, sys.argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
