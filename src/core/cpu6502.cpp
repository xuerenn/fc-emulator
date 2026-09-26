// cpu6502.cpp — 6502 指令集实现
#include "cpu6502.h"
#include "bus.h"

namespace fc {

CPU6502::CPU6502(Bus* bus) : bus_(bus) {}

// ---------------------------------------------------------------- 基础访问
u8 CPU6502::read(u16 addr) { return bus_->read(addr); }
void CPU6502::write(u16 addr, u8 value) { bus_->write(addr, value); }

u8 CPU6502::fetch() {
    if (!implied_) fetched_ = read(addrAbs_);
    return fetched_;
}

u16 CPU6502::read16(u16 addr) {
    u16 lo = read(addr);
    u16 hi = read(u16(addr + 1));
    return u16((hi << 8) | lo);
}

const char* CPU6502::currentOpName() const { return table()[opcode_].name; }

// ---------------------------------------------------------------- 指令表
const CPU6502::Instruction* CPU6502::table() {
#define I(o, a, c, im, n) { &CPU6502::ins_##o, &CPU6502::am_##a, c, im, n }
    static const Instruction t[256] = {
        I(BRK, IMM, 7, false, "BRK"), I(ORA, IZX, 6, false, "ORA"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, IZX, 8, false, "NOP*"),
        I(NOP, ZP0, 3, false, "NOP*"), I(ORA, ZP0, 3, false, "ORA"), I(ASL, ZP0, 5, false, "ASL"), I(NOP, ZP0, 5, false, "NOP*"),
        I(PHP, IMP, 3, true, "PHP"), I(ORA, IMM, 2, false, "ORA"), I(ASL, ACC, 2, true, "ASL"), I(NOP, IMM, 2, false, "NOP*"),
        I(NOP, ABS, 4, false, "NOP*"), I(ORA, ABS, 4, false, "ORA"), I(ASL, ABS, 6, false, "ASL"), I(NOP, ABS, 6, false, "NOP*"),
        I(BPL, REL, 2, false, "BPL"), I(ORA, IZY, 5, false, "ORA"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, IZY, 8, false, "NOP*"),
        I(NOP, ZPX, 4, false, "NOP*"), I(ORA, ZPX, 4, false, "ORA"), I(ASL, ZPX, 6, false, "ASL"), I(NOP, ZPX, 6, false, "NOP*"),
        I(CLC, IMP, 2, true, "CLC"), I(ORA, ABY, 4, false, "ORA"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, ABY, 7, false, "NOP*"),
        I(NOP, ABX, 4, false, "NOP*"), I(ORA, ABX, 4, false, "ORA"), I(ASL, ABX, 7, false, "ASL"), I(NOP, ABX, 7, false, "NOP*"),
        I(JSR, ABS, 6, false, "JSR"), I(AND, IZX, 6, false, "AND"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, IZX, 8, false, "NOP*"),
        I(BIT, ZP0, 3, false, "BIT"), I(AND, ZP0, 3, false, "AND"), I(ROL, ZP0, 5, false, "ROL"), I(NOP, ZP0, 5, false, "NOP*"),
        I(PLP, IMP, 4, true, "PLP"), I(AND, IMM, 2, false, "AND"), I(ROL, ACC, 2, true, "ROL"), I(NOP, IMM, 2, false, "NOP*"),
        I(BIT, ABS, 4, false, "BIT"), I(AND, ABS, 4, false, "AND"), I(ROL, ABS, 6, false, "ROL"), I(NOP, ABS, 6, false, "NOP*"),
        I(BMI, REL, 2, false, "BMI"), I(AND, IZY, 5, false, "AND"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, IZY, 8, false, "NOP*"),
        I(NOP, ZPX, 4, false, "NOP*"), I(AND, ZPX, 4, false, "AND"), I(ROL, ZPX, 6, false, "ROL"), I(NOP, ZPX, 6, false, "NOP*"),
        I(SEC, IMP, 2, true, "SEC"), I(AND, ABY, 4, false, "AND"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, ABY, 7, false, "NOP*"),
        I(NOP, ABX, 4, false, "NOP*"), I(AND, ABX, 4, false, "AND"), I(ROL, ABX, 7, false, "ROL"), I(NOP, ABX, 7, false, "NOP*"),
        I(RTI, IMP, 6, true, "RTI"), I(EOR, IZX, 6, false, "EOR"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, IZX, 8, false, "NOP*"),
        I(NOP, ZP0, 3, false, "NOP*"), I(EOR, ZP0, 3, false, "EOR"), I(LSR, ZP0, 5, false, "LSR"), I(NOP, ZP0, 5, false, "NOP*"),
        I(PHA, IMP, 3, true, "PHA"), I(EOR, IMM, 2, false, "EOR"), I(LSR, ACC, 2, true, "LSR"), I(NOP, IMM, 2, false, "NOP*"),
        I(JMP, ABS, 3, false, "JMP"), I(EOR, ABS, 4, false, "EOR"), I(LSR, ABS, 6, false, "LSR"), I(NOP, ABS, 6, false, "NOP*"),
        I(BVC, REL, 2, false, "BVC"), I(EOR, IZY, 5, false, "EOR"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, IZY, 8, false, "NOP*"),
        I(NOP, ZPX, 4, false, "NOP*"), I(EOR, ZPX, 4, false, "EOR"), I(LSR, ZPX, 6, false, "LSR"), I(NOP, ZPX, 6, false, "NOP*"),
        I(CLI, IMP, 2, true, "CLI"), I(EOR, ABY, 4, false, "EOR"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, ABY, 7, false, "NOP*"),
        I(NOP, ABX, 4, false, "NOP*"), I(EOR, ABX, 4, false, "EOR"), I(LSR, ABX, 7, false, "LSR"), I(NOP, ABX, 7, false, "NOP*"),
        I(RTS, IMP, 6, true, "RTS"), I(ADC, IZX, 6, false, "ADC"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, IZX, 8, false, "NOP*"),
        I(NOP, ZP0, 3, false, "NOP*"), I(ADC, ZP0, 3, false, "ADC"), I(ROR, ZP0, 5, false, "ROR"), I(NOP, ZP0, 5, false, "NOP*"),
        I(PLA, IMP, 4, true, "PLA"), I(ADC, IMM, 2, false, "ADC"), I(ROR, ACC, 2, true, "ROR"), I(NOP, IMM, 2, false, "NOP*"),
        I(JMP, IND, 5, false, "JMP"), I(ADC, ABS, 4, false, "ADC"), I(ROR, ABS, 6, false, "ROR"), I(NOP, ABS, 6, false, "NOP*"),
        I(BVS, REL, 2, false, "BVS"), I(ADC, IZY, 5, false, "ADC"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, IZY, 8, false, "NOP*"),
        I(NOP, ZPX, 4, false, "NOP*"), I(ADC, ZPX, 4, false, "ADC"), I(ROR, ZPX, 6, false, "ROR"), I(NOP, ZPX, 6, false, "NOP*"),
        I(SEI, IMP, 2, true, "SEI"), I(ADC, ABY, 4, false, "ADC"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, ABY, 7, false, "NOP*"),
        I(NOP, ABX, 4, false, "NOP*"), I(ADC, ABX, 4, false, "ADC"), I(ROR, ABX, 7, false, "ROR"), I(NOP, ABX, 7, false, "NOP*"),
        I(NOP, IMM, 2, false, "NOP*"), I(STA, IZX, 6, false, "STA"), I(NOP, IMM, 2, false, "NOP*"), I(NOP, IZX, 6, false, "NOP*"),
        I(STY, ZP0, 3, false, "STY"), I(STA, ZP0, 3, false, "STA"), I(STX, ZP0, 3, false, "STX"), I(NOP, ZP0, 3, false, "NOP*"),
        I(DEY, IMP, 2, true, "DEY"), I(NOP, IMM, 2, false, "NOP*"), I(TXA, IMP, 2, true, "TXA"), I(NOP, IMM, 2, false, "NOP*"),
        I(STY, ABS, 4, false, "STY"), I(STA, ABS, 4, false, "STA"), I(STX, ABS, 4, false, "STX"), I(NOP, ABS, 4, false, "NOP*"),
        I(BCC, REL, 2, false, "BCC"), I(STA, IZY, 6, false, "STA"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, IZY, 6, false, "NOP*"),
        I(STY, ZPX, 4, false, "STY"), I(STA, ZPX, 4, false, "STA"), I(STX, ZPY, 4, false, "STX"), I(NOP, ZPY, 4, false, "NOP*"),
        I(TYA, IMP, 2, true, "TYA"), I(STA, ABY, 5, false, "STA"), I(TXS, IMP, 2, true, "TXS"), I(NOP, ABY, 5, false, "NOP*"),
        I(NOP, ABX, 5, false, "NOP*"), I(STA, ABX, 5, false, "STA"), I(NOP, ABY, 5, false, "NOP*"), I(NOP, ABY, 5, false, "NOP*"),
        I(LDY, IMM, 2, false, "LDY"), I(LDA, IZX, 6, false, "LDA"), I(LDX, IMM, 2, false, "LDX"), I(NOP, IZX, 6, false, "NOP*"),
        I(LDY, ZP0, 3, false, "LDY"), I(LDA, ZP0, 3, false, "LDA"), I(LDX, ZP0, 3, false, "LDX"), I(NOP, ZP0, 3, false, "NOP*"),
        I(TAY, IMP, 2, true, "TAY"), I(LDA, IMM, 2, false, "LDA"), I(TAX, IMP, 2, true, "TAX"), I(NOP, IMM, 2, false, "NOP*"),
        I(LDY, ABS, 4, false, "LDY"), I(LDA, ABS, 4, false, "LDA"), I(LDX, ABS, 4, false, "LDX"), I(NOP, ABS, 4, false, "NOP*"),
        I(BCS, REL, 2, false, "BCS"), I(LDA, IZY, 5, false, "LDA"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, IZY, 5, false, "NOP*"),
        I(LDY, ZPX, 4, false, "LDY"), I(LDA, ZPX, 4, false, "LDA"), I(LDX, ZPY, 4, false, "LDX"), I(NOP, ZPY, 4, false, "NOP*"),
        I(CLV, IMP, 2, true, "CLV"), I(LDA, ABY, 4, false, "LDA"), I(TSX, IMP, 2, true, "TSX"), I(NOP, ABY, 4, false, "NOP*"),
        I(LDY, ABX, 4, false, "LDY"), I(LDA, ABX, 4, false, "LDA"), I(LDX, ABY, 4, false, "LDX"), I(NOP, ABY, 4, false, "NOP*"),
        I(CPY, IMM, 2, false, "CPY"), I(CMP, IZX, 6, false, "CMP"), I(NOP, IMM, 2, false, "NOP*"), I(NOP, IZX, 6, false, "NOP*"),
        I(CPY, ZP0, 3, false, "CPY"), I(CMP, ZP0, 3, false, "CMP"), I(DEC, ZP0, 5, false, "DEC"), I(NOP, ZP0, 5, false, "NOP*"),
        I(INY, IMP, 2, true, "INY"), I(CMP, IMM, 2, false, "CMP"), I(DEX, IMP, 2, true, "DEX"), I(NOP, IMM, 2, false, "NOP*"),
        I(CPY, ABS, 4, false, "CPY"), I(CMP, ABS, 4, false, "CMP"), I(DEC, ABS, 6, false, "DEC"), I(NOP, ABS, 6, false, "NOP*"),
        I(BNE, REL, 2, false, "BNE"), I(CMP, IZY, 5, false, "CMP"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, IZY, 5, false, "NOP*"),
        I(NOP, ZPX, 4, false, "NOP*"), I(CMP, ZPX, 4, false, "CMP"), I(DEC, ZPX, 6, false, "DEC"), I(NOP, ZPX, 6, false, "NOP*"),
        I(CLD, IMP, 2, true, "CLD"), I(CMP, ABY, 4, false, "CMP"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, ABY, 4, false, "NOP*"),
        I(NOP, ABX, 4, false, "NOP*"), I(CMP, ABX, 4, false, "CMP"), I(DEC, ABX, 7, false, "DEC"), I(NOP, ABX, 7, false, "NOP*"),
        I(CPX, IMM, 2, false, "CPX"), I(SBC, IZX, 6, false, "SBC"), I(NOP, IMM, 2, false, "NOP*"), I(NOP, IZX, 6, false, "NOP*"),
        I(CPX, ZP0, 3, false, "CPX"), I(SBC, ZP0, 3, false, "SBC"), I(INC, ZP0, 5, false, "INC"), I(NOP, ZP0, 5, false, "NOP*"),
        I(INX, IMP, 2, true, "INX"), I(SBC, IMM, 2, false, "SBC"), I(NOP, IMP, 2, true, "NOP"), I(SBC, IMM, 2, false, "SBC*"),
        I(CPX, ABS, 4, false, "CPX"), I(SBC, ABS, 4, false, "SBC"), I(INC, ABS, 6, false, "INC"), I(NOP, ABS, 6, false, "NOP*"),
        I(BEQ, REL, 2, false, "BEQ"), I(SBC, IZY, 5, false, "SBC"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, IZY, 5, false, "NOP*"),
        I(NOP, ZPX, 4, false, "NOP*"), I(SBC, ZPX, 4, false, "SBC"), I(INC, ZPX, 6, false, "INC"), I(NOP, ZPX, 6, false, "NOP*"),
        I(SED, IMP, 2, true, "SED"), I(SBC, ABY, 4, false, "SBC"), I(NOP, IMP, 2, true, "NOP*"), I(NOP, ABY, 4, false, "NOP*"),
        I(NOP, ABX, 4, false, "NOP*"), I(SBC, ABX, 4, false, "SBC"), I(INC, ABX, 7, false, "INC"), I(NOP, ABX, 7, false, "NOP*"),
    };
#undef I
    return t;
}

// ---------------------------------------------------------------- 运行
void CPU6502::reset() {
    a_ = x_ = y_ = 0;
    sp_ = 0xFD;
    status_ = u8(IF | UF);
    addrAbs_ = addrRel_ = 0;
    fetched_ = 0;
    opcode_ = 0;
    implied_ = false;
    stall_ = 0;
    pc_ = read16(0xFFFC);
    cycles_ = 7;
}

void CPU6502::irq() {
    if (getFlag(IF)) return;
    write(u16(0x0100 + sp_), u8(pc_ >> 8)); sp_--;
    write(u16(0x0100 + sp_), u8(pc_ & 0xFF)); sp_--;
    setFlag(BF, false);
    setFlag(UF, true);
    write(u16(0x0100 + sp_), status_); sp_--;
    setFlag(IF, true);
    pc_ = read16(0xFFFE);
    cycles_ = 7;
}

void CPU6502::nmi() {
    write(u16(0x0100 + sp_), u8(pc_ >> 8)); sp_--;
    write(u16(0x0100 + sp_), u8(pc_ & 0xFF)); sp_--;
    setFlag(BF, false);
    setFlag(UF, true);
    write(u16(0x0100 + sp_), status_); sp_--;
    setFlag(IF, true);
    pc_ = read16(0xFFFA);
    cycles_ = 7;
}

void CPU6502::clock() {
    if (stall_ > 0) {          // 总线被 DMA 占用
        stall_--;
        totalCycles_++;
        return;
    }
    if (cycles_ == 0) {
        opcode_ = read(pc_++);
        const Instruction& ins = table()[opcode_];
        implied_  = ins.implied;
        cycles_   = ins.cycles;
        setFlag(UF, true);
        u8 extra1 = (this->*ins.addr)();
        u8 extra2 = (this->*ins.op)();
        cycles_ = u8(cycles_ + (extra1 & extra2));
        setFlag(UF, true);
    }
    cycles_--;
    totalCycles_++;
}

// ---------------------------------------------------------------- 寻址
u8 CPU6502::am_IMP() { fetched_ = a_; return 0; }
u8 CPU6502::am_ACC() { fetched_ = a_; return 0; }
u8 CPU6502::am_IMM() { addrAbs_ = pc_++; return 0; }
u8 CPU6502::am_ZP0() { addrAbs_ = u16(read(pc_++) & 0x00FF); return 0; }
u8 CPU6502::am_ZPX() { addrAbs_ = u16((read(pc_++) + x_) & 0x00FF); return 0; }
u8 CPU6502::am_ZPY() { addrAbs_ = u16((read(pc_++) + y_) & 0x00FF); return 0; }
u8 CPU6502::am_REL() {
    addrRel_ = read(pc_++);
    if (addrRel_ & 0x80) addrRel_ |= 0xFF00;
    return 0;
}
u8 CPU6502::am_ABS() {
    u16 lo = read(pc_++);
    u16 hi = read(pc_++);
    addrAbs_ = u16((hi << 8) | lo);
    return 0;
}
u8 CPU6502::am_ABX() {
    u16 lo = read(pc_++);
    u16 hi = read(pc_++);
    addrAbs_ = u16(((hi << 8) | lo) + x_);
    return ((addrAbs_ & 0xFF00) != (hi << 8)) ? 1 : 0;
}
u8 CPU6502::am_ABY() {
    u16 lo = read(pc_++);
    u16 hi = read(pc_++);
    addrAbs_ = u16(((hi << 8) | lo) + y_);
    return ((addrAbs_ & 0xFF00) != (hi << 8)) ? 1 : 0;
}
u8 CPU6502::am_IND() {
    u16 lo = read(pc_++);
    u16 hi = read(pc_++);
    u16 ptr = u16((hi << 8) | lo);
    // 6502 著名的 JMP (xxxx) 跨页缺陷
    if ((ptr & 0x00FF) == 0x00FF)
        addrAbs_ = u16((read(u16(ptr & 0xFF00)) << 8) | read(ptr));
    else
        addrAbs_ = u16((read(u16(ptr + 1)) << 8) | read(ptr));
    return 0;
}
u8 CPU6502::am_IZX() {
    u16 t  = read(pc_++);
    u16 lo = read(u16((t + x_) & 0x00FF));
    u16 hi = read(u16((t + x_ + 1) & 0x00FF));
    addrAbs_ = u16((hi << 8) | lo);
    return 0;
}
u8 CPU6502::am_IZY() {
    u16 t  = read(pc_++);
    u16 lo = read(u16(t & 0x00FF));
    u16 hi = read(u16((t + 1) & 0x00FF));
    addrAbs_ = u16(((hi << 8) | lo) + y_);
    return ((addrAbs_ & 0xFF00) != (hi << 8)) ? 1 : 0;
}

// ---------------------------------------------------------------- 指令
u8 CPU6502::ins_ADC() {
    fetched_ = fetch();
    u16 sum = u16(a_) + u16(fetched_) + (getFlag(CF) ? 1 : 0);
    setFlag(CF, sum > 0xFF);
    setFlag(VF, (~(u16(a_) ^ u16(fetched_)) & (u16(a_) ^ sum) & 0x0080) != 0);
    a_ = u8(sum & 0xFF);
    updateZN(a_);
    return 1;
}
u8 CPU6502::ins_SBC() {
    fetched_ = fetch();
    u16 value = u16(fetched_) ^ 0x00FF;
    u16 sum = u16(a_) + value + (getFlag(CF) ? 1 : 0);
    setFlag(CF, sum > 0xFF);
    setFlag(VF, ((sum ^ u16(a_)) & (sum ^ value) & 0x0080) != 0);
    a_ = u8(sum & 0xFF);
    updateZN(a_);
    return 1;
}
u8 CPU6502::ins_AND() { fetched_ = fetch(); a_ = u8(a_ & fetched_); updateZN(a_); return 1; }
u8 CPU6502::ins_ORA() { fetched_ = fetch(); a_ = u8(a_ | fetched_); updateZN(a_); return 1; }
u8 CPU6502::ins_EOR() { fetched_ = fetch(); a_ = u8(a_ ^ fetched_); updateZN(a_); return 1; }

u8 CPU6502::ins_ASL() {
    fetched_ = fetch();
    u16 r = u16(fetched_) << 1;
    setFlag(CF, (r & 0x0100) != 0);
    updateZN(u8(r & 0xFF));
    if (implied_) a_ = u8(r & 0xFF); else write(addrAbs_, u8(r & 0xFF));
    return 0;
}
u8 CPU6502::ins_LSR() {
    fetched_ = fetch();
    setFlag(CF, (fetched_ & 0x01) != 0);
    u8 r = u8(fetched_ >> 1);
    updateZN(r);
    if (implied_) a_ = r; else write(addrAbs_, r);
    return 0;
}
u8 CPU6502::ins_ROL() {
    fetched_ = fetch();
    u16 r = u16(u16(fetched_) << 1) | (getFlag(CF) ? 1 : 0);
    setFlag(CF, (r & 0x0100) != 0);
    updateZN(u8(r & 0xFF));
    if (implied_) a_ = u8(r & 0xFF); else write(addrAbs_, u8(r & 0xFF));
    return 0;
}
u8 CPU6502::ins_ROR() {
    fetched_ = fetch();
    u16 r = u16((getFlag(CF) ? 0x80 : 0x00) | (fetched_ >> 1));
    setFlag(CF, (fetched_ & 0x01) != 0);
    updateZN(u8(r & 0xFF));
    if (implied_) a_ = u8(r & 0xFF); else write(addrAbs_, u8(r & 0xFF));
    return 0;
}

u8 CPU6502::ins_BIT() {
    fetched_ = fetch();
    u8 t = u8(a_ & fetched_);
    setFlag(ZF, t == 0);
    setFlag(NF, (fetched_ & 0x80) != 0);
    setFlag(VF, (fetched_ & 0x40) != 0);
    return 0;
}
u8 CPU6502::ins_CMP() { fetched_ = fetch(); u16 t = u16(a_) - u16(fetched_); setFlag(CF, t < 0x100); updateZN(u8(t & 0xFF)); return 1; }
u8 CPU6502::ins_CPX() { fetched_ = fetch(); u16 t = u16(x_) - u16(fetched_); setFlag(CF, t < 0x100); updateZN(u8(t & 0xFF)); return 0; }
u8 CPU6502::ins_CPY() { fetched_ = fetch(); u16 t = u16(y_) - u16(fetched_); setFlag(CF, t < 0x100); updateZN(u8(t & 0xFF)); return 0; }

u8 CPU6502::ins_DEC() { fetched_ = fetch(); u8 t = u8(fetched_ - 1); write(addrAbs_, t); updateZN(t); return 0; }
u8 CPU6502::ins_INC() { fetched_ = fetch(); u8 t = u8(fetched_ + 1); write(addrAbs_, t); updateZN(t); return 0; }
u8 CPU6502::ins_DEX() { x_ = u8(x_ - 1); updateZN(x_); return 0; }
u8 CPU6502::ins_DEY() { y_ = u8(y_ - 1); updateZN(y_); return 0; }
u8 CPU6502::ins_INX() { x_ = u8(x_ + 1); updateZN(x_); return 0; }
u8 CPU6502::ins_INY() { y_ = u8(y_ + 1); updateZN(y_); return 0; }

u8 CPU6502::ins_LDA() { fetched_ = fetch(); a_ = fetched_; updateZN(a_); return 1; }
u8 CPU6502::ins_LDX() { fetched_ = fetch(); x_ = fetched_; updateZN(x_); return 1; }
u8 CPU6502::ins_LDY() { fetched_ = fetch(); y_ = fetched_; updateZN(y_); return 1; }

u8 CPU6502::ins_STA() { write(addrAbs_, a_); return 0; }
u8 CPU6502::ins_STX() { write(addrAbs_, x_); return 0; }
u8 CPU6502::ins_STY() { write(addrAbs_, y_); return 0; }

u8 CPU6502::ins_JMP() { pc_ = addrAbs_; return 0; }
u8 CPU6502::ins_JSR() {
    pc_--;
    write(u16(0x0100 + sp_), u8(pc_ >> 8)); sp_--;
    write(u16(0x0100 + sp_), u8(pc_ & 0xFF)); sp_--;
    pc_ = addrAbs_;
    return 0;
}
u8 CPU6502::ins_RTS() {
    sp_++;
    u16 lo = read(u16(0x0100 + sp_)); sp_++;
    u16 hi = read(u16(0x0100 + sp_));
    pc_ = u16(((hi << 8) | lo) + 1);
    return 0;
}
u8 CPU6502::ins_RTI() {
    sp_++;
    status_ = read(u16(0x0100 + sp_));
    setFlag(BF, false);
    setFlag(UF, true);
    sp_++;
    u16 lo = read(u16(0x0100 + sp_)); sp_++;
    u16 hi = read(u16(0x0100 + sp_));
    pc_ = u16((hi << 8) | lo);
    return 0;
}
u8 CPU6502::ins_BRK() {
    pc_++;
    write(u16(0x0100 + sp_), u8(pc_ >> 8)); sp_--;
    write(u16(0x0100 + sp_), u8(pc_ & 0xFF)); sp_--;
    setFlag(BF, true);
    write(u16(0x0100 + sp_), status_); sp_--;
    setFlag(IF, true);
    pc_ = read16(0xFFFE);
    return 0;
}

u8 CPU6502::ins_PHA() { write(u16(0x0100 + sp_), a_); sp_--; return 0; }
u8 CPU6502::ins_PHP() { write(u16(0x0100 + sp_), u8(status_ | BF | UF)); sp_--; return 0; }
u8 CPU6502::ins_PLA() { sp_++; a_ = read(u16(0x0100 + sp_)); updateZN(a_); return 0; }
u8 CPU6502::ins_PLP() { sp_++; status_ = read(u16(0x0100 + sp_)); setFlag(UF, true); return 0; }

u8 CPU6502::ins_TAX() { x_ = a_; updateZN(x_); return 0; }
u8 CPU6502::ins_TAY() { y_ = a_; updateZN(y_); return 0; }
u8 CPU6502::ins_TSX() { x_ = sp_; updateZN(x_); return 0; }
u8 CPU6502::ins_TXA() { a_ = x_; updateZN(a_); return 0; }
u8 CPU6502::ins_TXS() { sp_ = x_; return 0; }
u8 CPU6502::ins_TYA() { a_ = y_; updateZN(a_); return 0; }

u8 CPU6502::ins_CLC() { setFlag(CF, false); return 0; }
u8 CPU6502::ins_CLD() { setFlag(DF, false); return 0; }
u8 CPU6502::ins_CLI() { setFlag(IF, false); return 0; }
u8 CPU6502::ins_CLV() { setFlag(VF, false); return 0; }
u8 CPU6502::ins_SEC() { setFlag(CF, true); return 0; }
u8 CPU6502::ins_SED() { setFlag(DF, true); return 0; }
u8 CPU6502::ins_SEI() { setFlag(IF, true); return 0; }

u8 CPU6502::ins_NOP() { if (!implied_) fetched_ = read(addrAbs_); return 0; }

// ---------------------------------------------------------------- 分支
#define BRANCH(cond)                                        \
    do {                                                    \
        if (cond) {                                         \
            cycles_++;                                      \
            addrAbs_ = u16(pc_ + addrRel_);                 \
            if ((addrAbs_ & 0xFF00) != (pc_ & 0xFF00)) cycles_++; \
            pc_ = addrAbs_;                                 \
        }                                                   \
        return 0;                                           \
    } while (0)

u8 CPU6502::ins_BCC() { BRANCH(!getFlag(CF)); }
u8 CPU6502::ins_BCS() { BRANCH(getFlag(CF)); }
u8 CPU6502::ins_BEQ() { BRANCH(getFlag(ZF)); }
u8 CPU6502::ins_BNE() { BRANCH(!getFlag(ZF)); }
u8 CPU6502::ins_BMI() { BRANCH(getFlag(NF)); }
u8 CPU6502::ins_BPL() { BRANCH(!getFlag(NF)); }
u8 CPU6502::ins_BVC() { BRANCH(!getFlag(VF)); }
u8 CPU6502::ins_BVS() { BRANCH(getFlag(VF)); }
#undef BRANCH

// ---------------------------------------------------------------- 快照
void CPU6502::save(StateWriter& w) const {
    w.u8v(a_); w.u8v(x_); w.u8v(y_); w.u8v(sp_);
    w.u8v(status_);
    w.u16v(pc_);
    w.u16v(addrAbs_); w.u16v(addrRel_);
    w.u8v(fetched_); w.u8v(opcode_); w.u8v(cycles_);
    w.u16v(stall_);
    w.boolv(implied_);
    w.u64v(totalCycles_);
}

void CPU6502::load(StateReader& r) {
    a_ = r.u8v(); x_ = r.u8v(); y_ = r.u8v(); sp_ = r.u8v();
    status_ = r.u8v();
    pc_ = r.u16v();
    addrAbs_ = r.u16v(); addrRel_ = r.u16v();
    fetched_ = r.u8v(); opcode_ = r.u8v(); cycles_ = r.u8v();
    stall_ = r.u16v();
    implied_ = r.boolv();
    totalCycles_ = r.u64v();
}

} // namespace fc
