// ppu.h — 2C02 PPU 图形管线
#pragma once

#include "types.h"

namespace fc {

class Cartridge;

// PPU 采用「逐点(dot)推进 + 逐行滚动快照」的混合模型：
//   * 时钟严格按 341 dots x 262 scanlines 推进，保证帧边界与 NMI 时序正确；
//   * 每个可见行在 dot 0 处冻结一份滚动状态（loopy v），像素直接在冻结状态上求解，
//     避免移位寄存器对齐偏差，同时天然支持逐行分屏（状态栏/菜单条）。
class PPU {
public:
    PPU();

    void connectCartridge(Cartridge* cart);
    void reset();

    void tick();

    u8   readRegister(u16 addr);
    void writeRegister(u16 addr, u8 value);

    // $4014 OAM DMA
    u8   readOAM(u16 addr) const { return oam_[addr & 0xFF]; }
    void writeOAM(u16 addr, u8 v) { oam_[addr & 0xFF] = v; }
    void setOAMAddr(u8 a) { oamAddr_ = a; }
    u8   oamAddr() const { return oamAddr_; }

    bool frameComplete() const { return frameComplete_; }
    void clearFrameComplete() { frameComplete_ = false; }
    bool nmiRequested() const { return nmiPending_; }
    void clearNmi() { nmiPending_ = false; }

    const u32* framebuffer() const { return framebuffer_.data(); }

    int scanline() const { return scanline_; }
    int dot() const { return dot_; }

    // 调试用只读访问（不经过镜像与缓冲，直接看原始显存）
    const u8* debugNametable() const { return nametable_; }
    int       debugNametableSize() const { return NT_SIZE; }
    const u8* debugPalette() const { return palette_; }
    u8        debugCtrl() const { return ctrl_; }
    u8        debugMask() const { return mask_; }

    // 调试：记录对 $2006/$2007 的访问，用于定位「游戏没写显存」还是「写错位置」
    struct TraceEntry { u16 addr; u8 val; bool isData; };
    void debugTrace(bool on, size_t maxEntries = 0) {
        traceOn_ = on;
        if (on) { traceMax_ = maxEntries; traceLog_.clear(); }   // 关闭时保留已记录的日志
    }
    const std::vector<TraceEntry>& debugTraceLog() const { return traceLog_; }

    void save(StateWriter& w) const;
    void load(StateReader& r);

private:
    static constexpr int NT_SIZE = 0x1000;   // 最多 4KB（支持四屏卡带）

    u8 readVRAM(u16 addr);
    void writeVRAM(u16 addr, u8 value);
    int mirrorNametable(int addr) const;
    static int mirrorPalette(int idx);

    void incrementX();
    void incrementY();
    void copyX();
    void copyY();
    void evaluateSprites(int targetScanline);
    void renderPixel();
    void traceRecord(u16 addr, u8 val, bool isData);

    // 寄存器
    u8 ctrl_ = 0;
    u8 mask_ = 0;
    u8 status_ = 0;
    u8 oamAddr_ = 0;
    u8 dataBuffer_ = 0;
    u8 fineX_ = 0;
    u16 v_ = 0;          // 当前 VRAM 地址（loopy v）
    u16 t_ = 0;          // 临时 VRAM 地址（loopy t）
    u16 lineV_ = 0;      // 当前扫描线冻结的滚动状态
    bool writeToggle_ = false;

    u8 palette_[32] = {};
    u8 nametable_[NT_SIZE] = {};
    u8 oam_[256] = {};
    std::array<u32, kScreenPixels> framebuffer_ = {};

    // 精灵行缓冲
    struct SpriteSlot {
        u8  x = 0;
        u8  attr = 0;
        u8  id = 0;
        u8  patternLo = 0;
        u8  patternHi = 0;
    };
    SpriteSlot sprites_[8];
    int spriteCount_ = 0;
    int sprite0Slot_ = -1;

    int  scanline_ = -1;   // -1 = 预渲染行
    int  dot_ = 0;
    bool frameComplete_ = false;
    bool nmiPending_ = false;

    Cartridge* cart_ = nullptr;

    // 调试追踪（不参与快照）
    bool traceOn_ = false;
    size_t traceMax_ = 0;
    std::vector<TraceEntry> traceLog_;
};

} // namespace fc
