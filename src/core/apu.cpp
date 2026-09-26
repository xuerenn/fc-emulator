// apu.cpp — 2A03 APU 实现
#include "apu.h"
#include "bus.h"

namespace fc {

namespace {

const u8 kDuty[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1},
};

const u8 kTriangle[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

const u8 kLengthTable[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};

// APU 周期数
const u16 kNoisePeriods[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};

// CPU 周期数
const u16 kDmcPeriods[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54,
};

} // namespace

APU::APU() { reset(); }

void APU::reset() {
    pulse1_ = Pulse{};
    pulse2_ = Pulse{};
    tri_    = Triangle{};
    noise_  = Noise{};
    noise_.lfsr = 1;
    dmc_    = Dmc{};
    frameCounter_ = 0;
    mode5_ = false;
    irqInhibit_ = false;
    frameIrq_ = false;
    cycleAccum_ = 0;
    pending_.clear();
    pending_.reserve(4096);
}

// ---------------------------------------------------------------- 主时钟
void APU::tick() {
    tickPulse(pulse1_, 1);
    tickPulse(pulse2_, 2);
    tickTriangle();
    tickNoise();
    tickDmc();

    frameCounter_++;
    if (mode5_) {
        if (frameCounter_ == 7457 || frameCounter_ == 22371) {
            quarterFrame();
        } else if (frameCounter_ == 14913 || frameCounter_ == 37281) {
            quarterFrame();
            halfFrame();
        } else if (frameCounter_ >= 37282) {
            frameCounter_ = 0;
        }
    } else {
        if (frameCounter_ == 7457 || frameCounter_ == 22371) {
            quarterFrame();
        } else if (frameCounter_ == 14913) {
            quarterFrame();
            halfFrame();
        } else if (frameCounter_ == 29829) {
            quarterFrame();
            halfFrame();
        } else if (frameCounter_ >= 29830) {
            frameCounter_ = 0;
            if (!irqInhibit_) frameIrq_ = true;
        }
    }

    cycleAccum_ += sampleRate_;
    if (cycleAccum_ >= int(kCpuClockHz)) {
        cycleAccum_ -= int(kCpuClockHz);
        pushSample();
    }
}

// ---------------------------------------------------------------- 方波
void APU::tickPulse(Pulse& p, int channel) {
    if (p.timer > 0) p.timer--;
    if (p.timer == 0) {
        p.timer = u16((p.period + 1) * 2);
        p.dutyPos = u8((p.dutyPos + 1) & 7);
    }

    u8 out = 0;
    if (p.enabled && p.length > 0 && p.period >= 8) {
        bool muted = false;
        if (p.sweepEnable) {
            const int change = int(p.period) >> p.sweepShift;
            const int target = p.sweepNeg
                ? int(p.period) - change - (channel == 1 ? 1 : 0)
                : int(p.period) + change;
            if (target > 0x7FF || target < 0) muted = true;
        }
        if (!muted && kDuty[p.duty][p.dutyPos]) out = p.volume;
    }
    p.out = out;
}

// ---------------------------------------------------------------- 三角波
void APU::tickTriangle() {
    Triangle& t = tri_;
    if (t.timer > 0) t.timer--;
    if (t.timer == 0) {
        t.timer = u16(t.period + 1);
        const bool audible = t.enabled && t.length > 0 && t.linear > 0;
        if (audible) t.seqPos = u8((t.seqPos + 1) & 31);
    }
    const bool audible = t.enabled && t.length > 0 && t.linear > 0;
    t.out = audible ? kTriangle[t.seqPos] : 0;
}

// ---------------------------------------------------------------- 噪声
void APU::tickNoise() {
    Noise& n = noise_;
    if (n.timer > 0) n.timer--;
    if (n.timer == 0) {
        n.timer = n.period;
        const u16 bit = n.mode ? u16(((n.lfsr >> 6) ^ n.lfsr) & 1)
                               : u16(((n.lfsr >> 1) ^ n.lfsr) & 1);
        n.lfsr = u16((n.lfsr >> 1) | (bit << 14));
    }
    n.out = (n.enabled && n.length > 0 && !(n.lfsr & 1)) ? n.volume : 0;
}

// ---------------------------------------------------------------- DMC
void APU::tickDmc() {
    Dmc& d = dmc_;
    if (!d.enabled) { d.sampleLeft = 0; d.level = 0; return; }

    if (d.bitsLeft == 0) {
        d.bitsLeft = 8;
        if (d.bufferFull) {
            d.shift = d.buffer;
            d.bufferFull = false;
            d.silence = false;
        } else {
            d.silence = true;
        }
        if (d.sampleLeft > 0) {
            d.buffer = bus_ ? bus_->read(d.addr) : u8(0);
            d.bufferFull = true;
            d.addr = u16(d.addr == 0xFFFF ? 0x8000 : d.addr + 1);
            d.sampleLeft--;
        } else if (d.loop) {
            d.addr = d.addrStart;
            d.sampleLeft = d.sampleLength;
        } else if (d.irqEnable) {
            frameIrq_ = true;
        }
    }

    if (d.timer > 0) {
        d.timer--;
    } else {
        d.timer = d.period;
        if (!d.silence) {
            if (d.shift & 1) { if (d.level <= 125) d.level = u8(d.level + 2); }
            else             { if (d.level >= 2)   d.level = u8(d.level - 2); }
        }
        d.shift = u8(d.shift >> 1);
        d.bitsLeft--;
    }
}

// ---------------------------------------------------------------- 帧序列器
void APU::quarterFrame() {
    auto env = [](bool& start, u8 volParam, u8& divider, u8& decay, bool halt, bool constVol, u8& volume) {
        if (start) {
            start = false;
            decay = 15;
            divider = volParam;
        } else if (divider == 0) {
            divider = volParam;
            if (decay > 0) decay--;
            else if (halt) decay = 15;
        } else {
            divider--;
        }
        volume = constVol ? volParam : decay;
    };

    env(pulse1_.envStart, pulse1_.volParam, pulse1_.envDivider, pulse1_.envDecay, pulse1_.lengthHalt != 0, pulse1_.constVol, pulse1_.volume);
    env(pulse2_.envStart, pulse2_.volParam, pulse2_.envDivider, pulse2_.envDecay, pulse2_.lengthHalt != 0, pulse2_.constVol, pulse2_.volume);
    env(noise_.envStart,  noise_.volParam,  noise_.envDivider,  noise_.envDecay,  noise_.lengthHalt != 0,  noise_.constVol,  noise_.volume);

    // 三角波线性计数器
    if (tri_.linearReload) tri_.linear = tri_.linearReload;
    else if (tri_.linear > 0) tri_.linear--;
    if (!tri_.enabled || (tri_.lengthHalt == 0 && tri_.linear == 0)) tri_.linear = 0;
    if (tri_.lengthHalt) tri_.linearReload = 0;
}

void APU::halfFrame() {
    auto len = [](u8& l, bool halt) { if (!halt && l > 0) l--; };

    len(pulse1_.length, pulse1_.lengthHalt != 0);
    len(pulse2_.length, pulse2_.lengthHalt != 0);
    len(tri_.length,    tri_.lengthHalt != 0);
    len(noise_.length,  noise_.lengthHalt != 0);

    sweepClock(pulse1_, 1);
    sweepClock(pulse2_, 2);
}

void APU::sweepClock(Pulse& p, int channel) {
    if (p.sweepDiv == 0 && p.sweepEnable && p.sweepShift > 0) {
        const int change = int(p.period) >> p.sweepShift;
        const int target = p.sweepNeg
            ? int(p.period) - change - (channel == 1 ? 1 : 0)
            : int(p.period) + change;
        if (target >= 0 && target <= 0x7FF && p.period >= 8) p.period = u16(target);
    }
    if (p.sweepDiv == 0 || p.sweepReload) {
        p.sweepDiv = p.sweepPeriod;
        p.sweepReload = false;
    } else {
        p.sweepDiv--;
    }
}

// ---------------------------------------------------------------- 寄存器
u8 APU::readStatus() {
    u8 v = 0;
    if (pulse1_.length > 0) v |= 0x01;
    if (pulse2_.length > 0) v |= 0x02;
    if (tri_.length > 0)    v |= 0x04;
    if (noise_.length > 0)  v |= 0x08;
    if (dmc_.sampleLeft > 0) v |= 0x10;
    if (frameIrq_)          v |= 0x40;
    frameIrq_ = false;
    return v;
}

void APU::writeRegister(u16 addr, u8 value) {
    switch (addr & 0x1F) {
        case 0x00: case 0x04: {
            Pulse& p = ((addr & 0x1F) == 0x00) ? pulse1_ : pulse2_;
            p.duty = u8((value >> 6) & 3);
            p.lengthHalt = u8((value >> 5) & 1);
            p.constVol = (value & 0x10) != 0;
            p.volParam = u8(value & 0x0F);
            break;
        }
        case 0x01: case 0x05: {
            Pulse& p = ((addr & 0x1F) == 0x01) ? pulse1_ : pulse2_;
            p.sweepEnable = (value & 0x80) != 0;
            p.sweepPeriod = u8((value >> 4) & 7);
            p.sweepNeg = (value & 0x08) != 0;
            p.sweepShift = u8(value & 7);
            p.sweepReload = true;
            break;
        }
        case 0x02: case 0x06: {
            Pulse& p = ((addr & 0x1F) == 0x02) ? pulse1_ : pulse2_;
            p.period = u16((p.period & 0x700) | value);
            break;
        }
        case 0x03: case 0x07: {
            Pulse& p = ((addr & 0x1F) == 0x03) ? pulse1_ : pulse2_;
            p.period = u16((p.period & 0x0FF) | ((u16(value) & 7) << 8));
            if (p.enabled) p.length = kLengthTable[value >> 3];
            p.dutyPos = 0;
            p.envStart = true;
            break;
        }
        case 0x08:
            tri_.lengthHalt = u8((value >> 7) & 1);
            tri_.linearReload = u8(value & 0x7F);
            break;
        case 0x0A:
            tri_.period = u16((tri_.period & 0x700) | value);
            break;
        case 0x0B:
            tri_.period = u16((tri_.period & 0x0FF) | ((u16(value) & 7) << 8));
            if (tri_.enabled) tri_.length = kLengthTable[value >> 3];
            tri_.linearReload = 1;
            break;
        case 0x0C:
            noise_.lengthHalt = u8((value >> 5) & 1);
            noise_.constVol = (value & 0x10) != 0;
            noise_.volParam = u8(value & 0x0F);
            break;
        case 0x0E:
            noise_.mode = (value & 0x80) != 0;
            noise_.period = u16(kNoisePeriods[value & 0x0F] * 2);
            break;
        case 0x0F:
            if (noise_.enabled) noise_.length = kLengthTable[value >> 3];
            noise_.envStart = true;
            break;
        case 0x10:
            dmc_.irqEnable = (value & 0x80) != 0;
            dmc_.loop = (value & 0x40) != 0;
            dmc_.period = kDmcPeriods[value & 0x0F];
            break;
        case 0x11:
            dmc_.level = u8(value & 0x7F);
            break;
        case 0x12:
            dmc_.addrStart = u16(0xC000 + (u16(value) * 64));
            break;
        case 0x13:
            dmc_.sampleLength = u16(u16(value) * 16 + 1);
            break;
        case 0x15: {
            pulse1_.enabled = (value & 0x01) != 0;
            pulse2_.enabled = (value & 0x02) != 0;
            tri_.enabled    = (value & 0x04) != 0;
            noise_.enabled  = (value & 0x08) != 0;
            const bool dmcEn = (value & 0x10) != 0;
            if (!pulse1_.enabled) pulse1_.length = 0;
            if (!pulse2_.enabled) pulse2_.length = 0;
            if (!tri_.enabled)    tri_.length = 0;
            if (!noise_.enabled)  noise_.length = 0;
            if (dmcEn && !dmc_.enabled) {
                if (dmc_.sampleLeft == 0) { dmc_.addr = dmc_.addrStart; dmc_.sampleLeft = dmc_.sampleLength; }
            } else if (!dmcEn) {
                dmc_.sampleLeft = 0;
            }
            dmc_.enabled = dmcEn;
            break;
        }
        case 0x17:
            mode5_ = (value & 0x80) != 0;
            irqInhibit_ = (value & 0x40) != 0;
            if (irqInhibit_) frameIrq_ = false;
            frameCounter_ = 0;
            if (mode5_) { quarterFrame(); halfFrame(); }
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- 输出
float APU::mix() const {
    const int p = int(pulse1_.out) + int(pulse2_.out);
    const float pulseOut = (p > 0) ? (95.88f / (8128.0f / float(p) + 100.0f)) : 0.0f;

    const float denom = float(tri_.out) / 8227.0f
                      + float(noise_.out) / 12241.0f
                      + float(dmc_.level) / 22638.0f;
    const float tndOut = (denom > 0.0f) ? (159.79f / (1.0f / denom + 100.0f)) : 0.0f;

    return pulseOut + tndOut;   // 约 0.0 ~ 1.0
}

void APU::pushSample() {
    float v = mix();
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    pending_.push_back(s16(v * 30000.0f));
}

void APU::takeSamples(std::vector<s16>& out) {
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

// ---------------------------------------------------------------- 快照
#define FC_SAVE_PULSE(p) \
    w.boolv(p.enabled); w.u8v(p.duty); w.u8v(p.dutyPos); \
    w.u16v(p.timer); w.u16v(p.period); \
    w.u8v(p.length); w.u8v(p.lengthHalt); w.boolv(p.constVol); \
    w.u8v(p.volParam); w.u8v(p.volume); w.u8v(p.envDivider); w.u8v(p.envDecay); w.boolv(p.envStart); \
    w.u8v(p.sweepShift); w.u8v(p.sweepPeriod); w.u8v(p.sweepDiv); \
    w.boolv(p.sweepNeg); w.boolv(p.sweepEnable); w.boolv(p.sweepReload); w.u8v(p.out)

#define FC_LOAD_PULSE(p) \
    p.enabled = r.boolv(); p.duty = r.u8v(); p.dutyPos = r.u8v(); \
    p.timer = r.u16v(); p.period = r.u16v(); \
    p.length = r.u8v(); p.lengthHalt = r.u8v(); p.constVol = r.boolv(); \
    p.volParam = r.u8v(); p.volume = r.u8v(); p.envDivider = r.u8v(); p.envDecay = r.u8v(); p.envStart = r.boolv(); \
    p.sweepShift = r.u8v(); p.sweepPeriod = r.u8v(); p.sweepDiv = r.u8v(); \
    p.sweepNeg = r.boolv(); p.sweepEnable = r.boolv(); p.sweepReload = r.boolv(); p.out = r.u8v()

void APU::save(StateWriter& w) const {
    FC_SAVE_PULSE(pulse1_);
    FC_SAVE_PULSE(pulse2_);
    w.boolv(tri_.enabled); w.u16v(tri_.timer); w.u16v(tri_.period);
    w.u8v(tri_.length); w.u8v(tri_.lengthHalt); w.u8v(tri_.linear); w.u8v(tri_.linearReload);
    w.u8v(tri_.seqPos); w.u8v(tri_.out);
    w.boolv(noise_.enabled); w.u16v(noise_.timer); w.u16v(noise_.period);
    w.u8v(noise_.length); w.u8v(noise_.lengthHalt); w.boolv(noise_.constVol);
    w.u8v(noise_.volParam); w.u8v(noise_.volume); w.u8v(noise_.envDivider); w.u8v(noise_.envDecay);
    w.boolv(noise_.envStart); w.u16v(noise_.lfsr); w.boolv(noise_.mode); w.u8v(noise_.out);
    w.boolv(dmc_.enabled); w.boolv(dmc_.irqEnable); w.boolv(dmc_.loop);
    w.u16v(dmc_.timer); w.u16v(dmc_.period); w.u16v(dmc_.addr); w.u16v(dmc_.addrStart);
    w.u16v(dmc_.sampleLength); w.u16v(dmc_.sampleLeft);
    w.u8v(dmc_.shift); w.u8v(dmc_.bitsLeft); w.u8v(dmc_.level);
    w.boolv(dmc_.bufferFull); w.u8v(dmc_.buffer); w.boolv(dmc_.silence);
    w.u16v(u16(frameCounter_)); w.boolv(mode5_); w.boolv(irqInhibit_); w.boolv(frameIrq_);
}

void APU::load(StateReader& r) {
    FC_LOAD_PULSE(pulse1_);
    FC_LOAD_PULSE(pulse2_);
    tri_.enabled = r.boolv(); tri_.timer = r.u16v(); tri_.period = r.u16v();
    tri_.length = r.u8v(); tri_.lengthHalt = r.u8v(); tri_.linear = r.u8v(); tri_.linearReload = r.u8v();
    tri_.seqPos = r.u8v(); tri_.out = r.u8v();
    noise_.enabled = r.boolv(); noise_.timer = r.u16v(); noise_.period = r.u16v();
    noise_.length = r.u8v(); noise_.lengthHalt = r.u8v(); noise_.constVol = r.boolv();
    noise_.volParam = r.u8v(); noise_.volume = r.u8v(); noise_.envDivider = r.u8v(); noise_.envDecay = r.u8v();
    noise_.envStart = r.boolv(); noise_.lfsr = r.u16v(); noise_.mode = r.boolv(); noise_.out = r.u8v();
    dmc_.enabled = r.boolv(); dmc_.irqEnable = r.boolv(); dmc_.loop = r.boolv();
    dmc_.timer = r.u16v(); dmc_.period = r.u16v(); dmc_.addr = r.u16v(); dmc_.addrStart = r.u16v();
    dmc_.sampleLength = r.u16v(); dmc_.sampleLeft = r.u16v();
    dmc_.shift = r.u8v(); dmc_.bitsLeft = r.u8v(); dmc_.level = r.u8v();
    dmc_.bufferFull = r.boolv(); dmc_.buffer = r.u8v(); dmc_.silence = r.boolv();
    frameCounter_ = r.u16v(); mode5_ = r.boolv(); irqInhibit_ = r.boolv(); frameIrq_ = r.boolv();
}

#undef FC_SAVE_PULSE
#undef FC_LOAD_PULSE

} // namespace fc
