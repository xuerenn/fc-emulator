// cpu6502.h — MOS 6502 (NES 2A03 内核) 模拟
#pragma once

#include "types.h"

namespace fc {

class Bus;

// 按 CPU 周期步进的 6502 实现。
// 设计要点：
//   * clock() 每次推进一个 CPU 周期，指令边界由 instructionComplete() 判定；
//   * 全部状态可存/可取快照，为网络回滚（rollback）预留能力。
class CPU6502 {
public:
    explicit CPU6502(Bus* bus);

    void reset();
    void irq();     // 可屏蔽中断（APU / Mapper 触发）
    void nmi();     // 不可屏蔽中断（PPU VBlank 触发）

    // 推进一个 CPU 周期
    void clock();

    // 总线被占用（OAM DMA 等）时插入空转周期
    void stall(u16 n) { stall_ += n; }

    bool instructionComplete() const { return cycles_ == 0 && stall_ == 0; }
    u64  totalCycles() const { return totalCycles_; }
    void setTotalCycles(u64 c) { totalCycles_ = c; }

    u16 pc() const { return pc_; }
    u8  a()  const { return a_; }
    u8  sp() const { return sp_; }
    u8  status() const { return u8(status_ | UF); }

    const char* currentOpName() const;

    // ------------------------------------------------------------ 状态快照
    struct State {
        u8  a = 0, x = 0, y = 0, sp = 0;
        u8  status = 0;
        u16 pc = 0;
        u16 addrAbs = 0, addrRel = 0;
        u8  fetched = 0;
        u8  opcode = 0;
        u8  cycles = 0;
        u64 totalCycles = 0;
        bool implied = false;
    };
    void save(StateWriter& w) const;
    void load(StateReader& r);

private:
    enum Flags : u8 {
        CF = 1 << 0, ZF = 1 << 1, IF = 1 << 2, DF = 1 << 3,
        BF = 1 << 4, UF = 1 << 5, VF = 1 << 6, NF = 1 << 7
    };

    using AddrFn = u8 (CPU6502::*)();
    using OpFn   = u8 (CPU6502::*)();

    struct Instruction {
        OpFn   op;
        AddrFn addr;
        u8     cycles;
        bool   implied;
        const char* name;
    };
    static const Instruction* table();

    // ------------------------------------------------------------ 基础访问
    u8   read(u16 addr);
    void write(u16 addr, u8 value);
    u8   fetch();
    u16  read16(u16 addr);

    void setFlag(u8 f, bool v) { if (v) status_ |= f; else status_ &= ~f; }
    bool getFlag(u8 f) const { return (status_ & f) != 0; }
    void updateZN(u8 v) { setFlag(ZF, v == 0); setFlag(NF, (v & 0x80) != 0); }

    // ------------------------------------------------------------ 寻址模式
    u8 am_IMP(); u8 am_ACC(); u8 am_IMM(); u8 am_ZP0(); u8 am_ZPX();
    u8 am_ZPY(); u8 am_REL(); u8 am_ABS(); u8 am_ABX(); u8 am_ABY();
    u8 am_IND(); u8 am_IZX(); u8 am_IZY();

    // ------------------------------------------------------------ 指令实现
    u8 ins_ADC(); u8 ins_AND(); u8 ins_ASL(); u8 ins_BCC(); u8 ins_BCS();
    u8 ins_BEQ(); u8 ins_BIT(); u8 ins_BMI(); u8 ins_BNE(); u8 ins_BPL();
    u8 ins_BRK(); u8 ins_BVC(); u8 ins_BVS(); u8 ins_CLC(); u8 ins_CLD();
    u8 ins_CLI(); u8 ins_CLV(); u8 ins_CMP(); u8 ins_CPX(); u8 ins_CPY();
    u8 ins_DEC(); u8 ins_DEX(); u8 ins_DEY(); u8 ins_EOR(); u8 ins_INC();
    u8 ins_INX(); u8 ins_INY(); u8 ins_JMP(); u8 ins_JSR(); u8 ins_LDA();
    u8 ins_LDX(); u8 ins_LDY(); u8 ins_LSR(); u8 ins_NOP(); u8 ins_ORA();
    u8 ins_PHA(); u8 ins_PHP(); u8 ins_PLA(); u8 ins_PLP(); u8 ins_ROL();
    u8 ins_ROR(); u8 ins_RTI(); u8 ins_RTS(); u8 ins_SBC(); u8 ins_SEC();
    u8 ins_SED(); u8 ins_SEI(); u8 ins_STA(); u8 ins_STX(); u8 ins_STY();
    u8 ins_TAX(); u8 ins_TAY(); u8 ins_TSX(); u8 ins_TXA(); u8 ins_TXS();
    u8 ins_TYA();

    u8 a_ = 0, x_ = 0, y_ = 0, sp_ = 0;
    u8 status_ = 0;
    u16 pc_ = 0;

    u16 addrAbs_ = 0, addrRel_ = 0;
    u8  fetched_ = 0;
    u8  opcode_ = 0;
    u8  cycles_ = 0;
    u16 stall_ = 0;
    bool implied_ = false;
    bool nmiPending_ = false;
    bool irqPending_ = false;
    u64 totalCycles_ = 0;

    Bus* bus_ = nullptr;
};

} // namespace fc
