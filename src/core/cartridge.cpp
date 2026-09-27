// cartridge.cpp — iNES 解析与 Mapper 实现
#include "cartridge.h"

#include "fs_utf8.h"

#include <cstdio>

namespace fc {

// ================================================================ NROM
u8 MapperNROM::readPRG(u16 addr) {
    if (prg_.empty()) return 0;
    if (prg_.size() == 0x4000) return prg_[addr & 0x3FFF];
    return prg_[addr & 0x7FFF];
}

u8 MapperNROM::readCHR(u16 addr) {
    if (chr_.empty()) return 0;
    return chr_[addr & 0x1FFF];
}

void MapperNROM::writeCHR(u16 addr, u8 v) {
    if (chrRam_ && !chr_.empty()) chr_[addr & 0x1FFF] = v;
}

// ================================================================ MMC1
void MapperMMC1::reset() {
    shift_ = 0x10;
    control_ = 0x0C;
    chrBank0_ = chrBank1_ = prgBank_ = 0;
}

u8 MapperMMC1::prgRead16(int bank16, u16 off) {
    const size_t banks = prg_.size() >> 14;
    if (banks == 0) return 0;
    const size_t b = size_t(bank16) % banks;
    return prg_[b * 0x4000 + (off & 0x3FFF)];
}

u8 MapperMMC1::readPRG(u16 addr) {
    if (prg_.empty()) return 0;
    const size_t banks16 = prg_.size() >> 14;
    if (banks16 == 0) return 0;
    const int mode = (control_ >> 2) & 3;
    const u16 off = u16(addr & 0x3FFF);

    switch (mode) {
        case 0:
        case 1: {
            // 32KB 连续切换，低 16KB 在 $8000，高 16KB 在 $C000
            const int base = (prgBank_ & 0x0E);
            return prgRead16(base + ((addr >= 0xC000) ? 1 : 0), off);
        }
        case 2:
            if (addr < 0xC000) return prgRead16(0, off);            // 固定第一块
            return prgRead16(prgBank_ & 0x0F, off);
        default:
            if (addr < 0xC000) return prgRead16(prgBank_ & 0x0F, off);
            return prgRead16(int(banks16) - 1, off);                 // 固定最后一块
    }
}

void MapperMMC1::writePRG(u16 addr, u8 v) {
    if (addr < 0x8000) return;
    if (v & 0x80) {                     // 复位序列端口
        shift_ = 0x10;
        control_ |= 0x0C;
        return;
    }
    const bool complete = (shift_ & 1) != 0;
    shift_ = u8((shift_ >> 1) | ((v & 1) << 4));
    if (!complete) return;

    const u8 val = u8(shift_ & 0x1F);
    switch ((addr >> 13) & 3) {
        case 0: control_  = val; break;
        case 1: chrBank0_ = val; break;
        case 2: chrBank1_ = val; break;
        default: prgBank_ = val; break;
    }
    shift_ = 0x10;
}

u8 MapperMMC1::chrRead4(int bank4, u16 off) {
    const size_t banks = chr_.size() >> 12;
    if (banks == 0) return 0;
    const size_t b = size_t(bank4) % banks;
    return chr_[b * 0x1000 + (off & 0x0FFF)];
}

u8 MapperMMC1::readCHR(u16 addr) {
    if (chr_.empty()) return 0;
    const u16 off = u16(addr & 0x0FFF);
    if (control_ & 0x10) {              // 4KB 模式
        return (addr < 0x1000) ? chrRead4(chrBank0_ & 0x1F, off)
                               : chrRead4(chrBank1_ & 0x1F, off);
    }
    const int base4 = (chrBank0_ & 0x1E);   // 8KB 模式：忽略最低位
    return chrRead4(base4 + ((addr >= 0x1000) ? 1 : 0), off);
}

void MapperMMC1::writeCHR(u16 addr, u8 v) {
    if (chrRam_ && !chr_.empty()) chr_[addr & 0x1FFF] = v;
}

int MapperMMC1::mirrorMode() const {
    switch (control_ & 3) {
        case 0: return MIRROR_ONE_LOW;
        case 1: return MIRROR_ONE_HIGH;
        case 2: return MIRROR_VERTICAL;
        default: return MIRROR_HORIZONTAL;
    }
}

void MapperMMC1::save(StateWriter& w) const {
    w.u8v(shift_); w.u8v(control_); w.u8v(chrBank0_); w.u8v(chrBank1_); w.u8v(prgBank_);
}

void MapperMMC1::load(StateReader& r) {
    shift_ = r.u8v(); control_ = r.u8v(); chrBank0_ = r.u8v(); chrBank1_ = r.u8v(); prgBank_ = r.u8v();
}

// ================================================================ UxROM
u8 MapperUxROM::readPRG(u16 addr) {
    const size_t banks = prg_.size() >> 14;
    if (banks == 0) return 0;
    if (addr < 0xC000) return prg_[(size_t(bank_) % banks) * 0x4000 + (addr & 0x3FFF)];
    return prg_[(banks - 1) * 0x4000 + (addr & 0x3FFF)];
}

void MapperUxROM::writePRG(u16 addr, u8 v) {
    if (addr >= 0x8000) bank_ = v;
}

u8 MapperUxROM::readCHR(u16 addr) {
    if (chr_.empty()) return 0;
    return chr_[addr & 0x1FFF];
}

void MapperUxROM::writeCHR(u16 addr, u8 v) {
    if (chrRam_ && !chr_.empty()) chr_[addr & 0x1FFF] = v;
}

// ================================================================ CNROM
u8 MapperCNROM::readPRG(u16 addr) {
    if (prg_.empty()) return 0;
    if (prg_.size() == 0x4000) return prg_[addr & 0x3FFF];
    return prg_[addr & 0x7FFF];
}

void MapperCNROM::writePRG(u16 addr, u8 v) {
    if (addr >= 0x8000) chrBank_ = u8(v & 0x03);
}

u8 MapperCNROM::readCHR(u16 addr) {
    const size_t banks = chr_.size() >> 13;
    if (banks == 0) return 0;
    const size_t b = size_t(chrBank_) % banks;
    return chr_[b * 0x2000 + (addr & 0x1FFF)];
}

void MapperCNROM::writeCHR(u16 addr, u8 v) {
    if (chrRam_ && !chr_.empty()) chr_[addr & 0x1FFF] = v;
}

// ================================================================ BMC 68-in-1 (58)
void MapperBMC68in1::writePRG(u16 addr, u8 v) {
    (void)v;                        // 数据总线不参与，寄存器值全部来自地址线
    if (addr < 0x8000) return;      // 掩码 $8000
    latch_ = u8(addr & 0xFF);
}

u8 MapperBMC68in1::readPRG(u16 addr) {
    if (prg_.empty()) return 0;
    const size_t banks16 = prg_.size() >> 14;      // 16KB 页数
    if (banks16 == 0) return 0;

    const int ppp = int(latch_ & 0x07);            // PRG A16..A14
    int page;
    if (nrom128()) {
        // NROM-128：16KB 单页，$8000 与 $C000 镜像同一块
        page = ppp;
    } else {
        // NROM-256：32KB 整块，块号取 PPP 的 bit2..bit1，块内高低半由 CPU A14 选择
        page = (ppp & 0x06) | ((addr >= 0xC000) ? 1 : 0);
    }
    const size_t b = size_t(page) % banks16;
    return prg_[b * 0x4000 + (addr & 0x3FFF)];
}

u8 MapperBMC68in1::readCHR(u16 addr) {
    if (chr_.empty()) return 0;
    if (chrRam_) return chr_[addr & 0x1FFF];
    const size_t banks8 = chr_.size() >> 13;       // 8KB 页数
    if (banks8 == 0) return 0;
    const size_t b = size_t((latch_ >> 3) & 0x07) % banks8;   // CHR A15..A13
    return chr_[b * 0x2000 + (addr & 0x1FFF)];
}

void MapperBMC68in1::writeCHR(u16 addr, u8 v) {
    if (chrRam_ && !chr_.empty()) chr_[addr & 0x1FFF] = v;
}

int MapperBMC68in1::mirrorMode() const {
    return (latch_ & 0x80) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL;
}

// ================================================================ Cartridge
void Cartridge::buildMapper() {
    switch (mapperId_) {
        case 0:  mapper_.reset(new MapperNROM(prg_, chr_, chrRam_, mirror_));  break;
        case 1:  mapper_.reset(new MapperMMC1(prg_, chr_, chrRam_, mirror_));  break;
        case 2:  mapper_.reset(new MapperUxROM(prg_, chr_, chrRam_, mirror_)); break;
        case 3:  mapper_.reset(new MapperCNROM(prg_, chr_, chrRam_, mirror_)); break;
        case 58: mapper_.reset(new MapperBMC68in1(prg_, chr_, chrRam_, mirror_)); break;
        default:
            // 未实现的 Mapper 回落到 NROM，保证仍能启动并给出提示
            std::fprintf(stderr,
                "[警告] Mapper %u 未实现，已回退为 NROM —— 分页/镜像不会生效，画面或声音可能异常。\n",
                unsigned(mapperId_));
            mapper_.reset(new MapperNROM(prg_, chr_, chrRam_, mirror_));
            break;
    }
}

bool Cartridge::loadFromFile(const std::string& path) {
    std::FILE* f = fc::fopenUtf8(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 16) { std::fclose(f); return false; }
    std::vector<u8> buf;
    buf.resize(size_t(n));
    const size_t got = std::fread(buf.data(), 1, size_t(n), f);
    std::fclose(f);
    buf.resize(got);

    // 用文件名做标题
    std::string label = path;
    const size_t slash = label.find_last_of("/\\");
    if (slash != std::string::npos) label = label.substr(slash + 1);
    return loadFromMemory(buf.data(), buf.size(), label);
}

bool Cartridge::loadFromMemory(const u8* data, size_t size, const std::string& label) {
    if (size < 16) return false;
    if (!(data[0] == 'N' && data[1] == 'E' && data[2] == 'S' && data[3] == 0x1A)) return false;

    const int prgBanks = data[4];
    const int chrBanks = data[5];
    const u8  flags6   = data[6];
    const u8  flags7   = data[7];

    mirror_   = (flags6 & 0x08) ? MIRROR_FOUR : ((flags6 & 0x01) ? MIRROR_VERTICAL : MIRROR_HORIZONTAL);
    battery_  = (flags6 & 0x02) != 0;
    const bool hasTrainer = (flags6 & 0x04) != 0;

    // Mapper 号：iNES 用 flags6 高 4 位 + flags7 高 4 位
    // NES 2.0 另有 4 位高位，但本项目支持 0-3，低 8 位已足够
    mapperId_ = u8((flags7 & 0xF0) | (flags6 >> 4));
    const bool nes20 = (flags7 & 0x0C) == 0x08;
    if (nes20 && (data[8] & 0x0F) != 0) {
        // 高 4 位非零说明是 256+ 的 Mapper，超出支持范围，回落处理
        mapperId_ = 0xFF;
    }

    size_t off = 16;
    if (hasTrainer) off += 512;

    const size_t prgSize = size_t(prgBanks) * 0x4000;
    const size_t chrSize = size_t(chrBanks) * 0x2000;
    if (off + prgSize + chrSize > size) return false;

    prg_.assign(data + off, data + off + prgSize);
    off += prgSize;

    chrRam_ = (chrBanks == 0);
    if (chrRam_) {
        chr_.assign(0x2000, 0);            // 8KB CHR RAM
    } else {
        chr_.assign(data + off, data + off + chrSize);
    }

    prgRam_.assign(0x2000, 0);

    title_ = label;
    buildMapper();
    return true;
}

void Cartridge::reset() {
    if (mapper_) mapper_->reset();
}

u8 Cartridge::readPRG(u16 addr) {
    if (addr >= 0x6000 && addr < 0x8000) return prgRam_.empty() ? 0 : prgRam_[addr & 0x1FFF];
    if (addr >= 0x8000 && mapper_) return mapper_->readPRG(addr);
    return 0;
}

void Cartridge::writePRG(u16 addr, u8 v) {
    if (addr >= 0x6000 && addr < 0x8000) {
        if (!prgRam_.empty()) prgRam_[addr & 0x1FFF] = v;
        return;
    }
    if (addr >= 0x8000 && mapper_) mapper_->writePRG(addr, v);
}

u8 Cartridge::readCHR(u16 addr) {
    if (addr < 0x2000 && mapper_) return mapper_->readCHR(addr);
    return 0;
}

void Cartridge::writeCHR(u16 addr, u8 v) {
    if (addr < 0x2000 && mapper_) mapper_->writeCHR(addr, v);
}

void Cartridge::save(StateWriter& w) const {
    w.u8v(mapperId_);
    w.u16v(u16(mirror_));
    if (mapper_) mapper_->save(w);
    w.raw(prgRam_.data(), prgRam_.size());
    if (chrRam_) w.raw(chr_.data(), chr_.size());
}

void Cartridge::load(StateReader& r) {
    mapperId_ = r.u8v();
    mirror_ = int(r.u16v());
    if (mapper_) mapper_->load(r);
    r.raw(prgRam_.data(), prgRam_.size());
    if (chrRam_) r.raw(chr_.data(), chr_.size());
}

} // namespace fc
