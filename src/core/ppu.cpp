// ppu.cpp — 2C02 PPU 实现
#include "ppu.h"
#include "cartridge.h"

namespace fc {

PPU::PPU() { reset(); }

void PPU::connectCartridge(Cartridge* cart) { cart_ = cart; }

void PPU::reset() {
    ctrl_ = mask_ = status_ = 0;
    oamAddr_ = 0;
    dataBuffer_ = 0;
    fineX_ = 0;
    v_ = t_ = lineV_ = 0;
    writeToggle_ = false;
    std::fill(palette_, palette_ + 32, u8(0));
    std::fill(oam_, oam_ + 256, u8(0));
    std::fill(nametable_, nametable_ + NT_SIZE, u8(0));
    framebuffer_.fill(kPaletteRGB[0]);
    scanline_ = -1;
    dot_ = 0;
    frameComplete_ = false;
    nmiPending_ = false;
    spriteCount_ = 0;
    sprite0Slot_ = -1;
}

// ---------------------------------------------------------------- 滚动寄存器
void PPU::incrementX() {
    if ((v_ & 0x001F) == 31) { v_ &= ~0x001F; v_ ^= 0x0400; }
    else v_ = u16(v_ + 1);
}

void PPU::incrementY() {
    if ((v_ & 0x7000) != 0x7000) {
        v_ = u16(v_ + 0x1000);
    } else {
        v_ &= ~0x7000;
        int y = (v_ & 0x03E0) >> 5;
        if (y == 29)      { y = 0; v_ ^= 0x0800; }
        else if (y == 31) { y = 0; }
        else              { y++; }
        v_ = u16((v_ & ~0x03E0) | (y << 5));
    }
}

void PPU::copyX() { v_ = u16((v_ & ~0x041F) | (t_ & 0x041F)); }
void PPU::copyY() { v_ = u16((v_ & ~0x7BE0) | (t_ & 0x7BE0)); }

// ---------------------------------------------------------------- 主时钟
void PPU::tick() {
    const bool busy = (scanline_ >= -1 && scanline_ < 240);

    // 只有渲染开启（BG 或精灵任一使能）时，PPU 才会在 dot 256/257/280-304 自动更新 v。
    // 渲染关闭时必须保持 v 原样 —— 否则用「关渲染 + 连续写 $2007」批量灌显存的游戏
    // 会在上传途中被 copyX/copyY 清掉地址，导致菜单/图形只写进一部分（花屏）。
    const bool rendering = (mask_ & 0x18) != 0;

    if (busy) {
        if (scanline_ == -1 && dot_ == 1) status_ &= ~0xE0;   // 预渲染行清除标志
        if (rendering) {
            if (dot_ == 0 && scanline_ >= 0) lineV_ = v_;      // 冻结本行滚动状态
            if (dot_ == 256) incrementY();
            if (dot_ == 257) {
                copyX();
                if (scanline_ < 239) evaluateSprites(scanline_ + 1);
            }
            if (scanline_ == -1 && dot_ >= 280 && dot_ < 305) copyY();
        }
    }

    if (scanline_ == 241 && dot_ == 1) {
        status_ |= 0x80;
        if (ctrl_ & 0x80) nmiPending_ = true;
        frameComplete_ = true;
    }

    if (scanline_ >= 0 && scanline_ < 240 && dot_ >= 1 && dot_ <= 256) renderPixel();

    dot_++;
    if (dot_ >= kDotsPerScanline) {
        dot_ = 0;
        scanline_++;
        if (scanline_ >= 261) scanline_ = -1;
    }
}

// ---------------------------------------------------------------- 精灵求值
void PPU::evaluateSprites(int target) {
    spriteCount_ = 0;
    sprite0Slot_ = -1;
    if (target < 0 || target > 239) return;

    const int height = (ctrl_ & 0x20) ? 16 : 8;
    for (int i = 0; i < 64; ++i) {
        const int sy  = oam_[i * 4 + 0];
        const int row = target - sy - 1;
        if (row < 0 || row >= height) continue;

        if (spriteCount_ >= 8) { status_ |= 0x20; continue; }  // 超 8 个：溢出标志

        SpriteSlot& s = sprites_[spriteCount_];
        const u8 attr = oam_[i * 4 + 2];
        const u8 tile = oam_[i * 4 + 1];
        int r = row;
        if (attr & 0x80) r = height - 1 - r;   // 垂直翻转

        u16 addr;
        if (height == 16) {
            const u16 base = u16(((tile & 0x01) ? 0x1000 : 0x0000) + ((u16(tile) & 0xFE) << 4));
            addr = (r < 8) ? u16(base + r) : u16(base + 16 + (r - 8));
        } else {
            const u16 base = u16(((ctrl_ & 0x08) ? 0x1000 : 0x0000) + (u16(tile) << 4));
            addr = u16(base + r);
        }

        s.x = oam_[i * 4 + 3];
        s.attr = attr;
        s.id = u8(i);
        s.patternLo = readVRAM(addr);
        s.patternHi = readVRAM(u16(addr + 8));
        if (i == 0) sprite0Slot_ = spriteCount_;
        spriteCount_++;
    }
}

// ---------------------------------------------------------------- 像素合成
void PPU::renderPixel() {
    const int x = dot_ - 1;
    const int y = scanline_;
    const bool renderBg = (mask_ & 0x08) != 0;
    const bool renderSp = (mask_ & 0x10) != 0;

    u8 bgPix = 0, bgPal = 0;
    if (renderBg) {
        const int px      = x + fineX_;
        const int tileCol = px >> 3;
        const int bitCol  = px & 7;
        int ntX = (lineV_ & 0x1F) + tileCol;
        const int ntY = (lineV_ >> 5) & 0x1F;
        int ntSel = (lineV_ >> 10) & 3;
        while (ntX >= 32) { ntX -= 32; ntSel ^= 1; }

        const int fineY = (lineV_ >> 12) & 7;
        const u8 tile = readVRAM(u16(0x2000 + (ntSel << 10) + (ntY << 5) + ntX));
        const u8 at   = readVRAM(u16(0x23C0 + (ntSel << 10) + ((ntY >> 2) << 3) + (ntX >> 2)));
        const int shift = ((ntY & 2) << 1) | (ntX & 2);
        bgPal = u8((at >> shift) & 0x03);

        const u16 patBase = u16((ctrl_ & 0x10) ? 0x1000 : 0x0000);
        const u16 addr    = u16(patBase + (u16(tile) << 4) + fineY);
        const u8 lo = readVRAM(addr);
        const u8 hi = readVRAM(u16(addr + 8));
        const int bit = 7 - bitCol;
        bgPix = u8((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
    }

    u8 spPix = 0, spPal = 0;
    bool spBehind = false, spIsZero = false;
    if (renderSp) {
        for (int i = 0; i < spriteCount_; ++i) {
            const SpriteSlot& s = sprites_[i];
            int off = x - int(s.x);
            if (off < 0 || off > 7) continue;
            if (s.attr & 0x40) off = 7 - off;      // 水平翻转
            const int bit = 7 - off;
            const u8 lo = u8((s.patternLo >> bit) & 1);
            const u8 hi = u8((s.patternHi >> bit) & 1);
            const u8 pix = u8((hi << 1) | lo);
            if (!pix) continue;
            spPix = pix;
            spPal = u8((s.attr & 0x03) | 0x04);    // 精灵调色板 4..7
            spBehind = (s.attr & 0x20) != 0;
            spIsZero = (s.id == 0);
            break;                                  // OAM 序号小者优先
        }
    }

    // 左 8 像素裁剪
    if (x < 8 && !(mask_ & 0x02)) bgPix = 0;
    if (x < 8 && !(mask_ & 0x04)) spPix = 0;

    // 精灵 0 命中
    if (renderBg && renderSp && spIsZero && bgPix != 0 && spPix != 0 && x < 255)
        status_ |= 0x40;

    u8 palIdx;
    if (bgPix == 0 && spPix == 0)      palIdx = 0;
    else if (spPix == 0)               palIdx = u8(bgPal * 4 + bgPix);
    else if (bgPix == 0)               palIdx = u8(spPal * 4 + spPix);
    else palIdx = spBehind ? u8(bgPal * 4 + bgPix) : u8(spPal * 4 + spPix);

    u8 color = u8(palette_[palIdx & 0x1F] & 0x3F);
    if (mask_ & 0x01) color &= 0x30;  // 灰度
    framebuffer_[size_t(y) * kScreenWidth + x] = kPaletteRGB[color & 0x3F];
}

// ---------------------------------------------------------------- 显存
int PPU::mirrorPalette(int idx) {
    idx &= 0x1F;
    if ((idx & 0x13) == 0x10) idx &= 0x0F;   // $3F10/$14/$18/$1C 镜像到 $3F00/04/08/0C
    return idx;
}

int PPU::mirrorNametable(int addr) const {
    const int off   = (addr - 0x2000) & 0x0FFF;
    int table       = (off >> 10) & 3;
    const int inner = off & 0x3FF;
    const int mode  = cart_ ? cart_->mirrorMode() : 1;
    switch (mode) {
        case 0: table = (table >> 1) & 1; break;  // 水平排列
        case 1: table = table & 1;        break;  // 垂直排列
        case 2: break;                            // 四屏
        case 3: table = 0; break;                 // 单屏低
        case 4: table = 1; break;                 // 单屏高
        default: table &= 1; break;
    }
    return table * 0x400 + inner;
}

u8 PPU::readVRAM(u16 addr) {
    addr &= 0x3FFF;
    if (addr >= 0x3F00) return palette_[mirrorPalette(addr & 0x1F)];
    if (addr < 0x2000)  return cart_ ? cart_->readCHR(addr) : u8(0);
    return nametable_[mirrorNametable(addr)];
}

void PPU::writeVRAM(u16 addr, u8 value) {
    addr &= 0x3FFF;
    if (addr >= 0x3F00) { palette_[mirrorPalette(addr & 0x1F)] = u8(value & 0x3F); return; }
    if (addr < 0x2000)  { if (cart_) cart_->writeCHR(addr, value); return; }
    nametable_[mirrorNametable(addr)] = value;
}

// ---------------------------------------------------------------- 寄存器
u8 PPU::readRegister(u16 addr) {
    switch (addr & 7) {
        case 2: {
            const u8 d = u8((status_ & 0xE0) | (dataBuffer_ & 0x1F));
            status_ &= u8(~0x80);
            writeToggle_ = false;
            return d;
        }
        case 4:
            return oam_[oamAddr_];
        case 7: {
            const u16 a = u16(v_ & 0x3FFF);
            u8 d;
            if (a >= 0x3F00) {
                d = palette_[mirrorPalette(a & 0x1F)];
                if (mask_ & 0x01) d &= 0x30;
                dataBuffer_ = readVRAM(u16(a - 0x1000));
            } else {
                d = dataBuffer_;
                dataBuffer_ = readVRAM(a);
            }
            v_ = u16(v_ + ((ctrl_ & 0x04) ? 32 : 1));
            return d;
        }
        default:
            return 0;
    }
}

void PPU::traceRecord(u16 addr, u8 val, bool isData) {
    if (!traceOn_ || traceLog_.size() >= traceMax_) return;
    traceLog_.push_back(TraceEntry{addr, val, isData});
}

void PPU::writeRegister(u16 addr, u8 value) {
    switch (addr & 7) {
        case 0:
            ctrl_ = value;
            t_ = u16((t_ & 0xF3FF) | ((u16(value) & 0x03) << 10));
            if (nmiPending_ && !(value & 0x80)) nmiPending_ = false;
            break;
        case 1:
            mask_ = value;
            break;
        case 3:
            oamAddr_ = value;
            break;
        case 4:
            oam_[oamAddr_] = value;
            oamAddr_++;
            break;
        case 5:
            if (!writeToggle_) {
                fineX_ = u8(value & 0x07);
                t_ = u16((t_ & 0xFFE0) | (u16(value) >> 3));
                writeToggle_ = true;
            } else {
                t_ = u16((t_ & 0x8FFF) | ((u16(value) & 0x07) << 12));
                t_ = u16((t_ & 0xFC1F) | ((u16(value) & 0xF8) << 2));
                writeToggle_ = false;
            }
            break;
        case 6:
            if (!writeToggle_) {
                t_ = u16((t_ & 0x00FF) | ((u16(value) & 0x3F) << 8));
                writeToggle_ = true;
            } else {
                t_ = u16((t_ & 0xFF00) | u16(value));
                v_ = t_;
                writeToggle_ = false;
                traceRecord(v_, 0, false);      // 记录一次「设置 VRAM 地址」
            }
            break;
        case 7: {
            traceRecord(u16(v_ & 0x3FFF), value, true);
            writeVRAM(u16(v_ & 0x3FFF), value);
            v_ = u16(v_ + ((ctrl_ & 0x04) ? 32 : 1));
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------- 快照
void PPU::save(StateWriter& w) const {
    w.u8v(ctrl_); w.u8v(mask_); w.u8v(status_); w.u8v(oamAddr_);
    w.u8v(dataBuffer_); w.u8v(fineX_);
    w.u16v(v_); w.u16v(t_); w.u16v(lineV_);
    w.boolv(writeToggle_);
    w.raw(palette_, 32);
    w.raw(oam_, 256);
    w.raw(nametable_, NT_SIZE);
    w.raw(framebuffer_.data(), framebuffer_.size() * sizeof(u32));
    w.u8v(u8(spriteCount_)); w.u8v(u8(spriteCount_ ? sprite0Slot_ + 1 : 0));
    for (int i = 0; i < spriteCount_; ++i) {
        w.u8v(sprites_[i].x); w.u8v(sprites_[i].attr); w.u8v(sprites_[i].id);
        w.u8v(sprites_[i].patternLo); w.u8v(sprites_[i].patternHi);
    }
    w.u16v(u16(scanline_ + 1)); w.u16v(u16(dot_));
    w.boolv(frameComplete_); w.boolv(nmiPending_);
}

void PPU::load(StateReader& r) {
    ctrl_ = r.u8v(); mask_ = r.u8v(); status_ = r.u8v(); oamAddr_ = r.u8v();
    dataBuffer_ = r.u8v(); fineX_ = r.u8v();
    v_ = r.u16v(); t_ = r.u16v(); lineV_ = r.u16v();
    writeToggle_ = r.boolv();
    r.raw(palette_, 32);
    r.raw(oam_, 256);
    r.raw(nametable_, NT_SIZE);
    r.raw(framebuffer_.data(), framebuffer_.size() * sizeof(u32));
    spriteCount_ = r.u8v();
    const u8 s0 = r.u8v();
    sprite0Slot_ = s0 ? int(s0) - 1 : -1;
    for (int i = 0; i < spriteCount_; ++i) {
        sprites_[i].x = r.u8v(); sprites_[i].attr = r.u8v(); sprites_[i].id = r.u8v();
        sprites_[i].patternLo = r.u8v(); sprites_[i].patternHi = r.u8v();
    }
    scanline_ = int(r.u16v()) - 1;
    dot_ = r.u16v();
    frameComplete_ = r.boolv(); nmiPending_ = r.boolv();
}

} // namespace fc
