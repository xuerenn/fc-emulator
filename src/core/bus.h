// bus.h — CPU 总线（内存映射 / PPU 与 APU 寄存器 / 手柄 / OAM DMA）
#pragma once

#include "types.h"
#include "cpu6502.h"
#include "ppu.h"
#include "apu.h"
#include "cartridge.h"
#include "controller.h"

namespace fc {

class Bus {
public:
    Bus();

    void connectCartridge(Cartridge* cart) { cart_ = cart; }
    void reset();

    u8   read(u16 addr);
    void write(u16 addr, u8 value);

    CPU6502&    cpu()       { return cpu_; }
    PPU&        ppu()       { return ppu_; }
    APU&        apu()       { return apu_; }
    Controller& player1()   { return ctrl1_; }
    Controller& player2()   { return ctrl2_; }

    const CPU6502& cpu() const { return cpu_; }
    const PPU&     ppu() const { return ppu_; }
    const APU&     apu() const { return apu_; }

    u32 systemClock() const { return systemClock_; }

    void save(StateWriter& w) const;
    void load(StateReader& r);

private:
    void oamDma(u8 page);

    std::array<u8, 2048> ram_ = {};
    CPU6502    cpu_;
    PPU        ppu_;
    APU        apu_;
    Controller ctrl1_, ctrl2_;
    Cartridge* cart_ = nullptr;
    u32        systemClock_ = 0;
};

} // namespace fc
