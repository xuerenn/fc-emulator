#!/usr/bin/env python3
"""把 iNES 卡带的每个 CHR bank 渲染成一张瓦片图，用于排查花屏。

用法: python dump_chr.py <rom.nes> <输出前缀> [放大倍数]

输出 <前缀>_chr0.png ... 每个 bank 一张图，瓦片按 16 列排列，
第 N 个瓦片（从 0 计）位于第 N//16 行、第 N%16 列。
颜色 0..3 渲染为 黑 / 深灰 / 浅灰 / 白。
"""
import struct
import sys
import zlib


def write_png(path, width, height, rgb):
    """rgb: bytes of length width*height*3"""
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


SHADES = [(0, 0, 0), (85, 85, 85), (170, 170, 170), (255, 255, 255)]


def render_bank(data, out_path, scale):
    ntiles = len(data) // 16
    cols = 16
    rows = (ntiles + cols - 1) // cols
    tw = 8 * scale
    W = cols * tw
    H = rows * tw
    buf = bytearray(W * H * 3)
    for t in range(ntiles):
        ox = (t % cols) * tw
        oy = (t // cols) * tw
        for y in range(8):
            lo = data[t * 16 + y]
            hi = data[t * 16 + y + 8]
            for x in range(8):
                bit = 7 - x
                px = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1)
                r, g, b = SHADES[px]
                for sy in range(scale):
                    base = ((oy + y * scale + sy) * W + ox + x * scale) * 3
                    for sx in range(scale):
                        buf[base + sx * 3 + 0] = r
                        buf[base + sx * 3 + 1] = g
                        buf[base + sx * 3 + 2] = b
    write_png(out_path, W, H, bytes(buf))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    rom_path, prefix = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 3

    with open(rom_path, "rb") as f:
        rom = f.read()
    if rom[:4] != b"NES\x1a":
        print("不是 iNES 文件")
        return 2

    prg_banks = rom[4]
    chr_banks = rom[5]
    flags6, flags7 = rom[6], rom[7]
    mapper = (flags7 & 0xF0) | (flags6 >> 4)
    has_trainer = (flags6 & 0x04) != 0

    off = 16 + (512 if has_trainer else 0)
    prg_size = prg_banks * 0x4000
    chr_size = chr_banks * 0x2000
    prg = rom[off:off + prg_size]
    chr_rom = rom[off + prg_size:off + prg_size + chr_size]

    print(f"Mapper      : {mapper}")
    print(f"PRG         : {prg_banks} x 16KB = {prg_size} 字节")
    print(f"CHR         : {chr_banks} x 8KB  = {chr_size} 字节")
    print(f"镜像        : {'四屏' if flags6 & 0x08 else ('垂直' if flags6 & 0x01 else '水平')}")
    print(f"电池        : {'有' if flags6 & 0x02 else '无'}")

    if not chr_rom:
        print("CHR 为 0（使用 CHR-RAM），无 bank 可导出")
        return 0

    for b in range(chr_banks):
        path = f"{prefix}_chr{b}.png"
        render_bank(chr_rom[b * 0x2000:(b + 1) * 0x2000], path, scale)
        print(f"  bank {b} -> {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
