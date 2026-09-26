// cartridge.h — iNES 卡带加载与 Mapper 分发
#pragma once

#include "types.h"

namespace fc {

// 与 PPU::mirrorNametable() 的约定一致
enum MirrorMode {
    MIRROR_HORIZONTAL = 0,
    MIRROR_VERTICAL   = 1,
    MIRROR_FOUR       = 2,
    MIRROR_ONE_LOW    = 3,
    MIRROR_ONE_HIGH   = 4,
};

// ---------------------------------------------------------------- Mapper 基类
class Mapper {
public:
    Mapper(std::vector<u8>& prg, std::vector<u8>& chr, bool chrRam, int mirror)
        : prg_(prg), chr_(chr), chrRam_(chrRam), mirror_(mirror) {}
    virtual ~Mapper() = default;

    virtual u8   readPRG(u16 addr) = 0;
    virtual void writePRG(u16 addr, u8 v) { (void)addr; (void)v; }
    virtual u8   readCHR(u16 addr) = 0;
    virtual void writeCHR(u16 addr, u8 v) { (void)addr; (void)v; }

    virtual void reset() {}
    virtual int  mirrorMode() const { return mirror_; }
    virtual bool irqPending() const { return false; }
    virtual void clearIrq() {}
    virtual void tickCpu() {}

    virtual const char* name() const = 0;

    virtual void save(StateWriter&) const {}
    virtual void load(StateReader&) {}

protected:
    std::vector<u8>& prg_;
    std::vector<u8>& chr_;
    bool chrRam_ = false;
    int  mirror_ = MIRROR_HORIZONTAL;
};

// ---------------------------------------------------------------- NROM (0)
class MapperNROM : public Mapper {
public:
    MapperNROM(std::vector<u8>& p, std::vector<u8>& c, bool cr, int m) : Mapper(p, c, cr, m) {}
    u8   readPRG(u16 addr) override;
    void writePRG(u16 addr, u8 v) override { (void)addr; (void)v; }
    u8   readCHR(u16 addr) override;
    void writeCHR(u16 addr, u8 v) override;
    const char* name() const override { return "NROM(0)"; }
};

// ---------------------------------------------------------------- MMC1 (1)
class MapperMMC1 : public Mapper {
public:
    MapperMMC1(std::vector<u8>& p, std::vector<u8>& c, bool cr, int m) : Mapper(p, c, cr, m) { reset(); }
    u8   readPRG(u16 addr) override;
    void writePRG(u16 addr, u8 v) override;
    u8   readCHR(u16 addr) override;
    void writeCHR(u16 addr, u8 v) override;
    void reset() override;
    int  mirrorMode() const override;
    const char* name() const override { return "MMC1(1)"; }
    void save(StateWriter& w) const override;
    void load(StateReader& r) override;

private:
    u8 prgRead16(int bank16, u16 off);
    u8 chrRead4(int bank4, u16 off);

    u8 shift_ = 0x10;
    u8 control_ = 0x0C;
    u8 chrBank0_ = 0;
    u8 chrBank1_ = 0;
    u8 prgBank_ = 0;
};

// ---------------------------------------------------------------- UxROM (2)
class MapperUxROM : public Mapper {
public:
    MapperUxROM(std::vector<u8>& p, std::vector<u8>& c, bool cr, int m) : Mapper(p, c, cr, m) { reset(); }
    u8   readPRG(u16 addr) override;
    void writePRG(u16 addr, u8 v) override;
    u8   readCHR(u16 addr) override;
    void writeCHR(u16 addr, u8 v) override;
    void reset() override { bank_ = 0; }
    const char* name() const override { return "UxROM(2)"; }
    void save(StateWriter& w) const override { w.u8v(bank_); }
    void load(StateReader& r) override { bank_ = r.u8v(); }

private:
    u8 bank_ = 0;
};

// ---------------------------------------------------------------- CNROM (3)
class MapperCNROM : public Mapper {
public:
    MapperCNROM(std::vector<u8>& p, std::vector<u8>& c, bool cr, int m) : Mapper(p, c, cr, m) { reset(); }
    u8   readPRG(u16 addr) override;
    void writePRG(u16 addr, u8 v) override;
    u8   readCHR(u16 addr) override;
    void writeCHR(u16 addr, u8 v) override;
    void reset() override { chrBank_ = 0; }
    const char* name() const override { return "CNROM(3)"; }
    void save(StateWriter& w) const override { w.u8v(chrBank_); }
    void load(StateReader& r) override { chrBank_ = r.u8v(); }

private:
    u8 chrBank_ = 0;
};

// ---------------------------------------------------------------- BMC 68-in-1 (58)
// 「N 合 1」简易合卡：寄存器值编码在地址总线上（写入 $8000-$FFFF，只取 A7..A0）
//   A~FEDC BA98 7654 3210
//     1... .... MSCC CPPP
//               |||| |+++-- PPP = PRG A16..A14
//               ||++-+------ CCC = CHR A15..A13
//               |+---------- 0=NROM-256(32KB, A14 由 CPU 决定) 1=NROM-128(16KB 镜像)
//               +----------- 0=垂直镜像 1=水平镜像
// 该类卡带被合卡菜单用来在 NROM/CNROM 小游戏之间切换，本 ROM(68in1_HKX5268) 即属此类。
class MapperBMC68in1 : public Mapper {
public:
    MapperBMC68in1(std::vector<u8>& p, std::vector<u8>& c, bool cr, int m) : Mapper(p, c, cr, m) { reset(); }
    u8   readPRG(u16 addr) override;
    void writePRG(u16 addr, u8 v) override;
    u8   readCHR(u16 addr) override;
    void writeCHR(u16 addr, u8 v) override;
    void reset() override { latch_ = 0; }
    int  mirrorMode() const override;
    const char* name() const override { return "BMC-68in1(58)"; }
    void save(StateWriter& w) const override { w.u8v(latch_); }
    void load(StateReader& r) override { latch_ = r.u8v(); }

private:
    bool nrom128() const { return (latch_ & 0x40) != 0; }
    u8   latch_ = 0;
};

// ---------------------------------------------------------------- 卡带
class Cartridge {
public:
    bool loadFromFile(const std::string& path);
    bool loadFromMemory(const u8* data, size_t size, const std::string& label);
    void reset();

    u8   readPRG(u16 addr);
    void writePRG(u16 addr, u8 v);
    u8   readCHR(u16 addr);
    void writeCHR(u16 addr, u8 v);

    int  mirrorMode() const { return mapper_ ? mapper_->mirrorMode() : mirror_; }
    bool irqPending() const { return mapper_ && mapper_->irqPending(); }
    void clearIrq() { if (mapper_) mapper_->clearIrq(); }
    void tickCpu() { if (mapper_) mapper_->tickCpu(); }

    u8  mapperId() const { return mapperId_; }
    const char* mapperName() const { return mapper_ ? mapper_->name() : "none"; }
    const std::string& title() const { return title_; }
    bool valid() const { return mapper_ != nullptr; }
    bool hasBattery() const { return battery_; }

    // 电池存档（.sav）
    std::vector<u8>& prgRam() { return prgRam_; }
    const std::vector<u8>& prgRam() const { return prgRam_; }

    void save(StateWriter& w) const;
    void load(StateReader& r);

private:
    void buildMapper();

    std::vector<u8> prg_;
    std::vector<u8> chr_;
    std::vector<u8> prgRam_;
    bool chrRam_ = false;
    bool battery_ = false;
    u8   mapperId_ = 0;
    int  mirror_ = MIRROR_HORIZONTAL;
    std::string title_;
    std::unique_ptr<Mapper> mapper_;
};

} // namespace fc
