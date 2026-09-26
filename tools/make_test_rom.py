#!/usr/bin/env python3
"""生成一个最小 NROM(Mapper 0) 测试 ROM。

用途：在没有真实 ROM 的情况下验证 fc-emulator 的全链路
（iNES 解析 -> CPU 复位/指令执行 -> Bus 寻址 -> PPU 调色板/图案/nametable -> 画面输出）。

画面内容：32x30 个方块格，顶边白色、其余边框橙色、内部黑色。
"""
import os
import sys

PRG_ORG = 0x8000
code = bytearray()


def b(*xs):
    code.extend(xs)


def label():
    return len(code)


def branch(op, target):
    """相对分支：偏移相对「下一条指令」。"""
    at = len(code)
    b(op, 0x00)
    off = target - len(code)
    code[at + 1] = off & 0xFF


def addr16(off):
    a = PRG_ORG + off
    return a & 0xFF, (a >> 8) & 0xFF


# ---------------------------------------------------------------- 代码
b(0x78)                  # SEI
b(0xD8)                  # CLD
b(0xA2, 0xFF)            # LDX #$FF
b(0x9A)                  # TXS
b(0xE8)                  # INX          -> X = 0
b(0x8E, 0x00, 0x20)      # STX $2000    NMI 关 / 图案表 $0000 / 增量 +1
b(0x8E, 0x01, 0x20)      # STX $2001    渲染关
b(0x8E, 0x10, 0x40)      # STX $4010    DMC IRQ 关

# 等第一次 VBlank
w1 = label()
b(0x2C, 0x02, 0x20)      # BIT $2002
branch(0x10, w1)         # BPL w1

# 写 32 字节调色板到 $3F00
b(0xA9, 0x3F)            # LDA #$3F
b(0x8D, 0x06, 0x20)      # STA $2006
b(0xA9, 0x00)            # LDA #$00
b(0x8D, 0x06, 0x20)      # STA $2006
b(0xA2, 0x00)            # LDX #$00
pl = label()
b(0xBD, 0x00, 0x00)      # LDA pal,X
pal_operand = len(code) - 2
b(0x8D, 0x07, 0x20)      # STA $2007
b(0xE8)                  # INX
b(0xE0, 0x20)            # CPX #$20
branch(0xD0, pl)         # BNE pl

# 清 1024 字节 nametable + 属性表（全 0 -> 全部使用 tile 0）
b(0xA9, 0x20)            # LDA #$20
b(0x8D, 0x06, 0x20)      # STA $2006
b(0xA9, 0x00)            # LDA #$00
b(0x8D, 0x06, 0x20)      # STA $2006
b(0xA9, 0x00)            # LDA #$00
b(0xA2, 0x00)            # LDX #$00
b(0xA0, 0x04)            # LDY #$04
nt = label()
b(0x8D, 0x07, 0x20)      # STA $2007
b(0xE8)                  # INX
branch(0xD0, nt)         # BNE nt
b(0x88)                  # DEY
branch(0xD0, nt)         # BNE nt

# 打开背景渲染（含左 8 列）
b(0xA9, 0x0A)            # LDA #$0A
b(0x8D, 0x01, 0x20)      # STA $2001

loop = label()
b(0x4C, 0x00, 0x00)      # JMP loop
jmp_operand = len(code) - 2

# ---------------------------------------------------------------- 数据
pal = label()
PALETTE = [
    0x0F, 0x27, 0x21, 0x30,      # 背景板0: 黑 / 橙 / 浅蓝 / 白
    0x0F, 0x16, 0x27, 0x30,
    0x0F, 0x1A, 0x2A, 0x30,
    0x0F, 0x12, 0x22, 0x30,
    0x0F, 0x27, 0x21, 0x30,      # 精灵板0（同样式，便于观察）
    0x0F, 0x16, 0x27, 0x30,
    0x0F, 0x1A, 0x2A, 0x30,
    0x0F, 0x12, 0x22, 0x30,
]
code.extend(PALETTE)

lo, hi = addr16(pal)
code[pal_operand] = lo
code[pal_operand + 1] = hi
lo, hi = addr16(loop)
code[jmp_operand] = lo
code[jmp_operand + 1] = hi

# ---------------------------------------------------------------- 组装 PRG
prg = bytearray(0x4000)
prg[0:len(code)] = code
prg[0x3FFA] = 0x00
prg[0x3FFB] = 0x80       # NMI   -> $8000
prg[0x3FFC] = 0x00
prg[0x3FFD] = 0x80       # RESET -> $8000
prg[0x3FFE] = 0x00
prg[0x3FFF] = 0x80       # IRQ   -> $8000

# ---------------------------------------------------------------- 组装 CHR
chrrom = bytearray(0x2000)
plane0 = [0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF]
plane1 = [0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
chrrom[0:8] = bytes(plane0)
chrrom[8:16] = bytes(plane1)
for r in range(8):
    chrrom[16 + r] = 0xAA if (r % 2 == 0) else 0x55
    chrrom[24 + r] = 0x00

# ---------------------------------------------------------------- iNES 头
header = bytearray(16)
header[0:4] = b"NES\x1a"
header[4] = 1        # 1 x 16KB PRG
header[5] = 1        # 1 x 8KB CHR
header[6] = 0x01     # 垂直镜像
header[7] = 0x00     # Mapper 0 (NROM)

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join("roms", "test.nes")
d = os.path.dirname(out)
if d:
    os.makedirs(d, exist_ok=True)
with open(out, "wb") as f:
    f.write(header)
    f.write(prg)
    f.write(chrrom)

print("已生成 %s" % out)
print("  PRG %d 字节 / CHR %d 字节 / 可执行代码 %d 字节" % (len(prg), len(chrrom), len(code)))
