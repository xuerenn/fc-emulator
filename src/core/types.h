// types.h — 全局基础类型与常量
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <algorithm>

namespace fc {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// ---------------------------------------------------------------- 硬件常量
constexpr int kScreenWidth  = 256;
constexpr int kScreenHeight = 240;
constexpr int kScreenPixels = kScreenWidth * kScreenHeight;

constexpr u32 kCpuClockHz   = 1789773;   // NTSC 2A03
constexpr u32 kPpuClockHz   = 5369318;   // = 3 x CPU
constexpr u32 kFrameCpuCycles = 29780;   // 一帧约 29780.5 个 CPU 周期
constexpr int kScanlinesPerFrame = 262;
constexpr int kDotsPerScanline   = 341;

// 一帧对应的 CPU 周期预算（浮点，供节流使用）
constexpr double kCpuCyclesPerFrame = 29780.5;

// ---------------------------------------------------------------- 2C02 主调色板
// 经典 NTSC 2C02 输出调色板（0xRRGGBB）
inline constexpr u32 kPaletteRGB[64] = {
    0x666666, 0x002A88, 0x1412A7, 0x3B00A4, 0x5C007E, 0x6E0040, 0x6C0600, 0x561D00,
    0x333500, 0x0B4800, 0x005200, 0x004F08, 0x00404D, 0x000000, 0x000000, 0x000000,
    0xADADAD, 0x155FD9, 0x4240FF, 0x7527FE, 0xA01ACC, 0xB71E7B, 0xB53120, 0x994E00,
    0x6B6D00, 0x388700, 0x0C9300, 0x008F32, 0x007C8D, 0x000000, 0x000000, 0x000000,
    0xFFFEFF, 0x64B0FF, 0x9290FF, 0xC676FF, 0xF36AFF, 0xFE6ECC, 0xFE8170, 0xEA9E22,
    0xBCBE00, 0x88D800, 0x5CE430, 0x45E082, 0x48CDDE, 0x4F4F4F, 0x000000, 0x000000,
    0xFFFEFF, 0xC0DFFF, 0xD3D2FF, 0xE8C8FF, 0xFBC2FF, 0xFEC4EA, 0xFECCC5, 0xF7D8A5,
    0xE4E594, 0xCFEF96, 0xBDF4AB, 0xB3F3CC, 0xB5EBF2, 0xB8B8B8, 0x000000, 0x000000,
};

inline u32 makeRGB(u8 r, u8 g, u8 b) {
    return (u32(r) << 16) | (u32(g) << 8) | u32(b);
}

// ---------------------------------------------------------------- 字节序小工具
// 状态快照的读写（回滚/存档共用），统一小端
class StateWriter {
public:
    void u8v(u8 v)  { buf_.push_back(v); }
    void u16v(u16 v) { buf_.push_back(u8(v & 0xFF)); buf_.push_back(u8(v >> 8)); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) buf_.push_back(u8((v >> (8 * i)) & 0xFF)); }
    void u64v(u64 v) { for (int i = 0; i < 8; ++i) buf_.push_back(u8((v >> (8 * i)) & 0xFF)); }
    void boolv(bool v) { buf_.push_back(v ? 1 : 0); }
    void raw(const void* p, size_t n) {
        const u8* q = static_cast<const u8*>(p);
        buf_.insert(buf_.end(), q, q + n);
    }
    std::vector<u8>&       data()       { return buf_; }
    const std::vector<u8>& data() const { return buf_; }
private:
    std::vector<u8> buf_;
};

class StateReader {
public:
    StateReader(const u8* p, size_t n) : p_(p), n_(n) {}
    u8  u8v()  { return n_ > 0 ? (--n_, *p_++) : 0; }
    u16 u16v() { u16 lo = u8v(); u16 hi = u8v(); return u16(lo | (hi << 8)); }
    u32 u32v() { u32 v = 0; for (int i = 0; i < 4; ++i) v |= u32(u8v()) << (8 * i); return v; }
    u64 u64v() { u64 v = 0; for (int i = 0; i < 8; ++i) v |= u64(u8v()) << (8 * i); return v; }
    bool boolv() { return u8v() != 0; }
    void raw(void* out, size_t n) {
        if (n_ < n) n = n_;
        std::memcpy(out, p_, n);
        p_ += n; n_ -= n;
    }
private:
    const u8* p_;
    size_t    n_;
};

} // namespace fc
