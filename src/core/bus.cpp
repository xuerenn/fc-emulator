// bus.cpp — 总线实现
#include "bus.h"

namespace fc {

Bus::Bus() : cpu_(this) {
    apu_.connectBus(this);
    ram_.fill(0);
}

void Bus::reset() {
    ram_.fill(0);
    cpu_.reset();
    ppu_.reset();
    apu_.reset();
    ctrl1_.reset();
    ctrl2_.reset();
    systemClock_ = 0;
}

u8 Bus::read(u16 addr) {
    if (addr <= 0x1FFF) return ram_[addr & 0x07FF];          // 2KB 内部 RAM（4 次镜像）
    if (addr <= 0x3FFF) return ppu_.readRegister(u16(addr & 0x0007));
    if (addr == 0x4015) return apu_.readStatus();
    if (addr == 0x4016) return ctrl1_.read();
    if (addr == 0x4017) return ctrl2_.read();
    if (addr >= 0x4000 && addr <= 0x401F) return 0;           // APU/IO 寄存器只写
    if (cart_) return cart_->readPRG(addr);
    return 0;
}

void Bus::write(u16 addr, u8 value) {
    if (addr <= 0x1FFF) { ram_[addr & 0x07FF] = value; return; }
    if (addr <= 0x3FFF) { ppu_.writeRegister(u16(addr & 0x0007), value); return; }
    if (addr == 0x4014) { oamDma(value); return; }
    if (addr == 0x4016) { ctrl1_.write(value); ctrl2_.write(value); return; }
    if (addr >= 0x4000 && addr <= 0x4017) { apu_.writeRegister(addr, value); return; }
    if (cart_) cart_->writePRG(addr, value);
}

void Bus::oamDma(u8 page) {
    const u16 base = u16(page) << 8;
    for (int i = 0; i < 256; ++i) {
        const u8 d = read(u16(base + u16(i)));
        ppu_.writeOAM(u8(i), d);
    }
    // DMA 期间 CPU 停摆 513 个周期（偶发 +1 的奇偶对齐这里从略）
    cpu_.stall(513);
}

void Bus::save(StateWriter& w) const {
    w.raw(ram_.data(), ram_.size());
    cpu_.save(w);
    ppu_.save(w);
    apu_.save(w);
    ctrl1_.save(w);
    ctrl2_.save(w);
    w.u32v(systemClock_);
}

void Bus::load(StateReader& r) {
    r.raw(ram_.data(), ram_.size());
    cpu_.load(r);
    ppu_.load(r);
    apu_.load(r);
    ctrl1_.load(r);
    ctrl2_.load(r);
    systemClock_ = r.u32v();
}

} // namespace fc
